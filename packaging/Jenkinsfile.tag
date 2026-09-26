// ============================================================================
// Jenkinsfile.tag -- qualify a release candidate and cut its tag
//
// The ONLY way a crete release tag is made. Pushing the tag is the release
// (the webhook starts Crete-Release: OBS, Homebrew, Windows, Codeberg), and a
// published tag is never moved, so everything is checked BEFORE it exists:
//
//   1. resolve COMMIT to a full SHA on main, check the Makefile at that SHA
//      says VERSION, and that vVERSION does not exist yet
//   2. run Crete-Weekly on EXACTLY that SHA (or reuse WEEKLY_BUILD, which must
//      have measured that SHA), and copy its qualification + measurements
//   3. fetch the previous release's measurements from its Codeberg release
//   4. scripts/tag_message.py composes the tag message and REFUSES if the
//      weekly's gate failed (any failed test / catastrophic delta / missing
//      suite) or if IMPACT says 'identical' while measured values moved
//   5. create the annotated tag on the SHA and push it (not in DRY_RUN)
//   6. attach the qualification, the measurements and the weekly's detailed
//      reports to the tag's Codeberg release -- the measurements are what the
//      NEXT release's bit-identity check compares against
//   7. start Crete-Site, which adds the release to the crete-dr changelog
//
// DRY_RUN defaults to ON: it does all of that except 5 and 6, and archives the
// exact message it would tag. The weekly takes ~2 h, so the usual flow is a dry
// run, read the message, then a real run with WEEKLY_BUILD set to the weekly the
// dry run started -- minutes, not hours, and it tags what you read.
//
// The tag message is the release record everything else reads: its subject is
// the Codeberg release title and the OBS changelog line, its body the release
// notes, and its trailers (Impact / Reproduce / Qualified-by) the crete-dr
// site's impact chips. See scripts/tag_message.py for the format.
//
// ── ONE-TIME JENKINS SETUP ──────────────────────────────────────────────────
// Job "Crete-Tag" in folder Crete: "Pipeline script from SCM", Git
//   https://codeberg.org/abksh/Crete.git, branch */main, Script Path
//   packaging/Jenkinsfile.tag, lightweight checkout ON, no triggers.
// Credential codeberg-release-token (Secret text, repository read+write): used
//   to push the tag over HTTPS and to attach files to the release.
// Crete-Weekly must grant copyArtifactPermission('/Crete/Crete-Tag') -- its
//   Jenkinsfile.weeklyRuns (crete-pytest) does.
// ============================================================================

import groovy.transform.Field

@Field def WEEKLY_JOB = 'Crete-Weekly'
@Field def SITE_JOB   = 'Crete-Site'

def isManual() {
    return !currentBuild.getBuildCauses('hudson.model.Cause$UserIdCause').isEmpty()
}

pipeline {
    agent { label 'bee' }

    options {
        disableConcurrentBuilds()
        // The weekly alone has a 4 h timeout.
        timeout(time: 6, unit: 'HOURS')
        buildDiscarder(logRotator(numToKeepStr: '30'))
        timestamps()
    }

    parameters {
        string(name: 'VERSION', defaultValue: '',
               description: 'Release version without the v, e.g. 0.19.6. The Makefile at COMMIT must already say VERSION ?= this.')
        string(name: 'TITLE', defaultValue: '',
               description: 'One line: what this release is. The tag subject becomes "crête VERSION — TITLE".')
        text(name: 'NOTES', defaultValue: '',
             description: 'What changed, as paragraphs separated by blank lines. Inline marks only: `code`, **strong**, *em*. Where numbers moved, say why.')
        string(name: 'IMPACT', defaultValue: '',
               description: 'Comma-separated: moves / identical / additive. "identical" is CHECKED against the previous release and blocks the tag if any value moved.')
        string(name: 'REPRODUCE', defaultValue: '',
               description: 'Required with "moves": the flag(s) that give back the previous numbers, e.g. --dr-blocks legacy.')
        string(name: 'COMMIT', defaultValue: 'main',
               description: 'Branch or SHA to release. Resolved to a full SHA at the start; must be on main.')
        string(name: 'WEEKLY_BUILD', defaultValue: '',
               description: 'Reuse this Crete-Weekly build instead of running a new one. It must have measured exactly the resolved SHA.')
        booleanParam(name: 'DRY_RUN', defaultValue: true,
               description: 'Qualify and compose the message, but do not tag, push or attach anything. Untick to release.')
    }

    environment {
        CODEBERG_REPO = 'abksh/Crete'
        TAGGER_NAME   = 'Ashwin Balaji'
        TAGGER_EMAIL  = 's.ashwinbalaji.de@gmail.com'
    }

    stages {
        stage('Resolve') {
            steps {
                script {
                    // Only a human click may release; a replay or an upstream
                    // trigger with a stale DRY_RUN=false must not.
                    env.DRY = (params.DRY_RUN || !isManual()) ? 'true' : 'false'
                    // Every later stage's shell reads the version from here.
                    env.P_VERSION = params.VERSION?.trim() ?: ''
                    writeFile file: '.tag/notes.md', text: params.NOTES ?: ''
                }
                withEnv(["P_TITLE=${params.TITLE}", "P_COMMIT=${params.COMMIT}"]) {
                    sh '''
set -eu
mkdir -p .tag
case "$P_VERSION" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "ERROR: VERSION must be X.Y.Z, got '$P_VERSION'"; exit 1 ;;
esac
[ -n "$P_TITLE" ] || { echo "ERROR: TITLE is empty"; exit 1; }
git fetch --quiet --force --tags origin '+refs/heads/*:refs/remotes/origin/*'
SHA=$(git rev-parse --verify --quiet "origin/$P_COMMIT^{commit}" || git rev-parse --verify --quiet "$P_COMMIT^{commit}" || true)
[ -n "$SHA" ] || { echo "ERROR: '$P_COMMIT' is not a branch or commit of $CODEBERG_REPO"; exit 1; }
git merge-base --is-ancestor "$SHA" origin/main || { echo "ERROR: $SHA is not on main"; exit 1; }
MK=$(git show "$SHA:Makefile" | sed -n 's/^VERSION *?= *//p')
[ "$MK" = "$P_VERSION" ] || { echo "ERROR: the Makefile at ${SHA%"${SHA#???????}"} says VERSION ?= $MK, not $P_VERSION -- bump and push first"; exit 1; }
if git ls-remote --exit-code --tags origin "refs/tags/v$P_VERSION" >/dev/null; then
    echo "ERROR: v$P_VERSION already exists on Codeberg; a published tag is never moved"; exit 1
fi
# The previous release: the highest vX.Y.Z below this one.
PREV=$(git tag -l 'v[0-9]*.[0-9]*.[0-9]*' | sed 's/^v//' | { cat; echo "$P_VERSION"; } | sort -V \
       | awk -v v="$P_VERSION" '$0 == v { print p; exit } { p = $0 }')
printf '%s\\n' "$SHA" > .tag/sha
printf '%s\\n' "$PREV" > .tag/prev
echo "releasing v$P_VERSION at $SHA (previous release: ${PREV:-none})"
'''
                }
                script {
                    env.RELEASE_SHA = readFile('.tag/sha').trim()
                    env.PREV_VERSION = readFile('.tag/prev').trim()
                    currentBuild.displayName = "#${env.BUILD_NUMBER} v${params.VERSION} ${env.RELEASE_SHA.take(7)}" +
                                               (env.DRY == 'true' ? ' (dry run)' : '')
                }
            }
        }

        stage('Qualify') {
            steps {
                script {
                    def n = params.WEEKLY_BUILD?.trim()
                    if (!n) {
                        // The same settings as the Monday cron, pinned to the SHA.
                        def w = build(job: WEEKLY_JOB, wait: true, propagate: false, parameters: [
                            string(name: 'CRETE_COMMIT', value: env.RELEASE_SHA),
                            booleanParam(name: 'RUN_SMOKE', value: true),
                            booleanParam(name: 'RUN_CROSSARCH', value: true),
                            string(name: 'DR_BLOCKS', value: 'reference'),
                            string(name: 'AGENT_LABEL', value: 'bee'),
                            booleanParam(name: 'FRESH_CORPUS', value: false),
                            booleanParam(name: 'CLEAN_WORKSPACE', value: false),
                            booleanParam(name: 'PUBLISH_BADGES', value: false)])
                        n = "${w.number}"
                        echo "Crete-Weekly #${n}: ${w.result} (${w.absoluteUrl})"
                        // The gate below decides. A weekly that finished at all
                        // left a qualification; one that did not cannot release.
                    }
                    env.WEEKLY_N = n
                    copyArtifacts(projectName: WEEKLY_JOB, selector: specific(n), target: '.tag/weekly',
                                  flatten: true, fingerprintArtifacts: true,
                                  filter: 'test-results/qualification.json, test-results/qualification.txt, ' +
                                          'test-results/measurements.json, test-results/*_detailed.txt')
                }
                sh '''
set -eu
python3 - <<'EOF'
import json, os, sys
q = json.load(open(".tag/weekly/qualification.json"))
want = os.environ["RELEASE_SHA"]
got = (q.get("crete") or {}).get("sha") or ""
if got != want:
    sys.exit(f"ERROR: Crete-Weekly #{q.get('weekly_build')} measured {got or 'an unrecorded commit'}, not {want}")
print(f"Crete-Weekly #{q['weekly_build']} measured {want}: gate ok={q['gate']['ok']}")
EOF
cat .tag/weekly/qualification.txt
'''
            }
        }

        stage('Compose') {
            steps {
                withEnv(["P_TITLE=${params.TITLE}", "P_IMPACT=${params.IMPACT}", "P_REPRODUCE=${params.REPRODUCE}"]) {
                    sh '''
set -eu
PREVM=""
if [ -n "$PREV_VERSION" ]; then
    # The previous release's measurements, from its Codeberg release (public).
    # No release at all (404) and a release without the asset both mean "none".
    REL=$(curl -fsS "https://codeberg.org/api/v1/repos/$CODEBERG_REPO/releases/tags/v$PREV_VERSION" 2>/dev/null || true)
    URL=""
    if [ -n "$REL" ]; then
        URL=$(printf '%s' "$REL" | python3 -c 'import json,sys; d=json.load(sys.stdin); n=sys.argv[1]
print(next((a["browser_download_url"] for a in d.get("assets", []) if a["name"] == n), ""))' "crete-$PREV_VERSION-measurements.json")
    fi
    if [ -n "$URL" ]; then
        curl -fsSL -o .tag/prev-measurements.json "$URL"
        PREVM="--previous-measurements .tag/prev-measurements.json"
    else
        echo "v$PREV_VERSION has no recorded measurements -- the impact cannot be checked this once"
    fi
fi
# shellcheck disable=SC2086
python3 scripts/tag_message.py --version "$P_VERSION" --title "$P_TITLE" \\
    --notes-file .tag/notes.md --impact "$P_IMPACT" --reproduce "$P_REPRODUCE" \\
    --qualification .tag/weekly/qualification.json \\
    --qualification-text .tag/weekly/qualification.txt \\
    --measurements .tag/weekly/measurements.json \\
    $PREVM --previous-version "${PREV_VERSION:-none}" --out .tag/message.txt
'''
                }
                archiveArtifacts artifacts: '.tag/message.txt', fingerprint: true
            }
        }

        stage('Tag and push') {
            when { environment name: 'DRY', value: 'false' }
            steps {
                withCredentials([string(credentialsId: 'codeberg-release-token', variable: 'CODEBERG_TOKEN')]) {
                    sh '''
set -eu
TAG="v$P_VERSION"
git tag -d "$TAG" >/dev/null 2>&1 || true
git -c user.name="$TAGGER_NAME" -c user.email="$TAGGER_EMAIL" \\
    tag -a --cleanup=verbatim -F .tag/message.txt "$TAG" "$RELEASE_SHA"
# The token reaches git through an askpass helper, never the URL or the log.
ASK=$(mktemp); trap 'rm -f "$ASK"' EXIT
printf '#!/bin/sh\\ncase "$1" in Username*) echo abksh ;; *) echo "$CODEBERG_TOKEN" ;; esac\\n' > "$ASK"
chmod 700 "$ASK"
GIT_ASKPASS="$ASK" GIT_TERMINAL_PROMPT=0 \\
    git push "https://codeberg.org/$CODEBERG_REPO.git" "refs/tags/$TAG"
echo "pushed $TAG -> the webhook starts Crete-Release"
'''
                }
            }
        }

        stage('Attach qualification') {
            when { environment name: 'DRY', value: 'false' }
            steps {
                withCredentials([string(credentialsId: 'codeberg-release-token', variable: 'CODEBERG_TOKEN')]) {
                    sh '''
set -eu
V="$P_VERSION"
cp .tag/weekly/measurements.json   ".tag/crete-$V-measurements.json"
cp .tag/weekly/qualification.json  ".tag/crete-$V-qualification.json"
(cd .tag/weekly && python3 -m zipfile -c "../crete-$V-weekly-report.zip" qualification.txt *_detailed.txt)
python3 scripts/codeberg_release.py --repo "$CODEBERG_REPO" --tag "v$V" --notes-file .tag/message.txt \\
    ".tag/crete-$V-measurements.json" ".tag/crete-$V-qualification.json" ".tag/crete-$V-weekly-report.zip"
'''
                }
            }
        }

        stage('Update site') {
            when { environment name: 'DRY', value: 'false' }
            steps {
                // Not waited on: the release is already out; the site entry
                // follows, and Crete-Site's daily run retries if this one fails.
                build(job: SITE_JOB, wait: false,
                      parameters: [string(name: 'TAG', value: "v${params.VERSION}")])
            }
        }
    }

    post {
        success {
            echo(env.DRY == 'true'
                 ? "DRY RUN: v${params.VERSION} was not tagged. The message is archived; re-run with DRY_RUN unticked and WEEKLY_BUILD=${env.WEEKLY_N} to release it."
                 : "v${params.VERSION} is tagged and pushed; Crete-Release takes it from here.")
        }
        always {
            sh 'rm -rf .tag/weekly .tag/prev-measurements.json'
        }
    }
}
