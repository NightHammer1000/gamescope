#!/usr/bin/env sh

# Install our own default scripts and looks.
#
# Scoped to share/telescope on purpose: this used to rm -rf share/gamescope,
# which would have deleted an upstream gamescope install's data on the way past.
mkdir -p "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/telescope"
rm -rf "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/telescope/scripts" || true
rm -rf "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/telescope/looks" || true
cp -r "${MESON_SOURCE_ROOT}/scripts" "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/telescope/scripts"
cp -r "${MESON_SOURCE_ROOT}/looks" "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/telescope/looks"
