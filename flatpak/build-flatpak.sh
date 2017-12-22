#!/bin/bash

flatpak-builder \
  --force-clean \
  --ccache \
  --require-changes \
  --repo=repo \
  --arch=$(flatpak --default-arch) \
  --subject="build of com.github.paolostivanin.OTPClient, $(date)" \
  build com.github.paolostivanin.OTPClient.json
