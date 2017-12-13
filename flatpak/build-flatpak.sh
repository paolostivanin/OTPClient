#!/bin/bash

flatpak-builder \
  --force-clean \
  --ccache \
  --require-changes \
  --repo=repo \
  --arch=$(flatpak --default-arch) \
  --subject="build of org.gnome.OTPClient, $(date)" \
  build org.gnome.OTPClient.json
