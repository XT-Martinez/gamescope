#!/usr/bin/bash
# Wrapper that injects --hdr-debug-force-output into the gamescope command line.
# The gamescope-session-plus script overwrites HDR_OPTIONS, so we can't pass
# this flag through env vars. This wrapper transparently adds it.
exec "$(dirname "$0")/gamescope" --hdr-debug-force-output "$@"
