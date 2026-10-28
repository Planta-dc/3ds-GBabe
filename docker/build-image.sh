#!/bin/bash
# Prints the build image for this checkout: the published devcontainer image,
# tagged with a hash of everything that defines it (the Dockerfile plus the
# packaging-tools script it runs). A commit that changes either gets a different
# tag, so CI never picks up an image that does not match its own sources.
#
#   bash docker/build-image.sh             -> ghcr.io/<owner>/gbabe-devcontainer:<hash>
#   bash docker/build-image.sh --pull      -> also make it available locally,
#                                             pulled, or built if not published
#   bash docker/build-image.sh --pull --push  -> build and publish it
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOCKERFILE="$ROOT/docker/gbabe-devcontainer.Dockerfile"
TOOLS="$ROOT/.github/scripts/install-3ds-packaging-tools.sh"

OWNER="${GITHUB_REPOSITORY_OWNER:-Mr-PauI}"
TAG="$(cat "$DOCKERFILE" "$TOOLS" | sha256sum | cut -c1-12)"
IMAGE="ghcr.io/${OWNER,,}/gbabe-devcontainer:${TAG}"

pull=0
push=0
for arg in "$@"; do
  case "$arg" in
    --pull) pull=1 ;;
    --push) push=1 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

if [ "$pull" = 1 ]; then
  if ! docker pull "$IMAGE" >&2; then
    echo "$IMAGE is not published (yet): building it locally" >&2
    docker build -f "$DOCKERFILE" -t "$IMAGE" "$ROOT" >&2
  fi
fi

if [ "$push" = 1 ]; then
  docker push "$IMAGE" >&2
fi

echo "$IMAGE"
