#!/usr/bin/env bash
# Redundancy failover on a cluster running the mxl-k8s mxlfs branch.
#
# Members of a video, an ANC and an audio set are produced on MEMBER1
# and MEMBER2, and read through the sets' outputs on READER. The run
# kills the member-1 producers, brings them back, then drains MEMBER2.
# Each reader prints its counters; a hitless run shows no invalid,
# wrong_index, late or silent count growing after start-up. The run
# fails when a set's reader had to reopen the output.
#
#   MEMBER1=node-a MEMBER2=node-b READER=node-c tests/cluster/failover.sh
#
# MEMBER1 set to the READER node restarts members on the reader's node,
# which recreate their flow directories in the domain the output's views
# are in.
# KUBECTL, IMAGE, NAMESPACE, WIDTH, HEIGHT, RATE (N or N/D), CHANNELS
# and SAMPLE_RATE override defaults.
# IMAGE needs setcheck at /usr/local/bin/setcheck, as the mxl-k8s
# demo-tools image of test/mxlfs-pr-env carries it.
# HOSTPATH=<dir> mounts the writers' domain from that node directory
# instead of through the CSI driver.
# PLAIN=1 adds a writer on MEMBER1 read on READER without a set.
set -euo pipefail

: "${MEMBER1:?}" "${MEMBER2:?}" "${READER:?}"
KUBECTL=${KUBECTL:-kubectl}
IMAGE=${IMAGE:-ghcr.io/danielclasen/mxlfs-setcheck:dev}
NAMESPACE=${NAMESPACE:-mxlfs-e2e}
WIDTH=${WIDTH:-1920}
HEIGHT=${HEIGHT:-1080}
RATE=${RATE:-50}
CHANNELS=${CHANNELS:-2}
SAMPLE_RATE=${SAMPLE_RATE:-48000}
k() { $KUBECTL "$@"; }
ts() { date -u +%H:%M:%S; }

id() { printf 'aaaaaaaa-0000-4000-8000-0000000000%s' "$1"; }

volume() { # access
	if [ "$1" = write ] && [ -n "${HOSTPATH:-}" ]; then
		printf '      hostPath:\n        path: %s\n        type: Directory\n' "$HOSTPATH"
	else
		printf '      csi:\n        driver: mxl.qvest-digital.com\n        volumeAttributes:\n          access: %s\n' "$1"
	fi
}

pod() { # name node access args...
	local name=$1 node=$2 access=$3
	shift 3
	local args
	args=$(printf '"%s",' "$@")
	cat <<EOF
apiVersion: v1
kind: Pod
metadata:
  name: $name
  namespace: $NAMESPACE
  labels: {app: setcheck, role: $name}
spec:
  nodeName: $node
  restartPolicy: Never
  terminationGracePeriodSeconds: 2
  containers:
    - name: setcheck
      image: $IMAGE
      imagePullPolicy: IfNotPresent
      command: ["/usr/local/bin/setcheck"]
      args: [${args%,}]
      securityContext:
        capabilities:
          add: ["IPC_LOCK", "SYS_RESOURCE"]
      volumeMounts:
        - name: mxl
          mountPath: /run/mxl/domain
  volumes:
    - name: mxl
$(volume "$access")
---
EOF
}

writer() { # name node id format member
	pod "$1" "$2" write write -id "$3" -format "$4" -member "$5" \
		-width "$WIDTH" -height "$HEIGHT" -rate "$RATE" \
		-channels "$CHANNELS" -sample-rate "$SAMPLE_RATE"
}

set_of() { # format digit
	local f=$1 d=$2
	writer "$f-member1" "$MEMBER1" "$(id "${d}1")" "$f" 1
	writer "$f-member2" "$MEMBER2" "$(id "${d}2")" "$f" 2
	cat <<EOF
apiVersion: mxl.qvest-digital.com/v1alpha1
kind: MxlFlowRedundancySet
metadata:
  name: $NAMESPACE-$f
spec:
  members:
    - flowID: $(id "${d}1")
    - flowID: $(id "${d}2")
  output:
    flowID: $(id "${d}0")
---
EOF
	pod "$f-reader" "$READER" read read -id "$(id "${d}0")"
}

manifests() {
	printf 'apiVersion: v1\nkind: Namespace\nmetadata:\n  name: %s\n  labels:\n    pod-security.kubernetes.io/enforce: privileged\n---\n' "$NAMESPACE"
	set_of video 1
	set_of data 2
	set_of audio 3
	if [ "${PLAIN:-}" = 1 ]; then
		writer plain-writer "$MEMBER1" "$(id 01)" video 1
		pod plain-reader "$READER" read read -id "$(id 01)"
	fi
}

members() { # which
	manifests | awk -v want="$1" 'BEGIN{RS="---\n"; ORS="---\n"} $0 ~ "name: [a-z]+-" want "\n"'
}

k delete ns "$NAMESPACE" --wait=true --ignore-not-found >/dev/null
for f in video data audio; do k delete mxlflowredundancyset "$NAMESPACE-$f" --ignore-not-found >/dev/null; done
manifests | k apply -f - >/dev/null
echo "$(ts) sets applied"
sleep 60
echo "$(ts) killing the member1 producers on $MEMBER1"
k -n "$NAMESPACE" delete pod video-member1 data-member1 audio-member1 --wait=true >/dev/null
sleep 45
echo "$(ts) restoring the member1 producers"
members member1 | k apply -f - >/dev/null
sleep 45
echo "$(ts) draining $MEMBER2"
k drain "$MEMBER2" --ignore-daemonsets --delete-emptydir-data --force --timeout=180s >/dev/null 2>&1 ||
	echo "$(ts) drain returned non-zero"
sleep 60
echo "$(ts) uncordoning $MEMBER2"
k uncordon "$MEMBER2" >/dev/null
readers="video data audio"
[ "${PLAIN:-}" = 1 ] && readers="$readers plain"
failed=0
for f in $readers; do
	echo "== $f-reader"
	logs=$(k -n "$NAMESPACE" logs "$f-reader") || {
		echo "$(ts) $f-reader has no logs"
		failed=1
		continue
	}
	# awk reads all of it: head would close the pipe under pipefail.
	printf '%s\n' "$logs" | awk '!/ ok=/ && n++ < 12'
	last=$(printf '%s\n' "$logs" | grep " ok=" | tail -1) || true
	if [ -z "$last" ]; then
		echo "$(ts) $f-reader printed no counters"
		failed=1
		continue
	fi
	echo "$last"
	# A plain flow's reader reopens when its writer comes back.
	case "$f:$last" in
	plain:* | *" reopened=0 "*) ;;
	*) failed=1 ;;
	esac
done
if [ "$failed" = 1 ]; then
	echo "$(ts) a reader of a set's output had to reopen it, or left no counters"
	exit 1
fi
