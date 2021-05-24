#!/bin/bash -x

set -o errexit -o nounset -o pipefail

sngrep_sources=/context/sngrep
resultdir=/context/result
workdir=$(mktemp -d)

get_dist() {
  grep -oP  "(?<=^ID=).*" /etc/os-release | sed -e 's/"//g'
}

build() {
  local dist=$1
  case ${dist} in
    rhel)
      create_srpm
      build_rpm
      ;;
    centos)
      create_srpm
      build_rpm
      ;;
    fedora)
      create_srpm
      build_rpm
      ;;
    debian)
      build_dep
      ;;
    opensuse-leap)
      create_srpm
      build_rpm
      ;;
    opensuse-tumbleweed)
      create_srpm
      build_rpm
      ;;
    alpine)
      build_apk
      ;;
  esac
}

create_srpm() {
  local rpm_topdir=${workdir}/rpmbuild
  local version=$(grep "Version:" pkg/rpm/SPECS/sngrep.spec | sed -E -e 's/Version:\s+//')

  echo "packaging sngrep version '${version}'"

  mkdir -p ${rpm_topdir}/SOURCES

  git archive \
    --format tar.gz \
    --prefix sngrep-${version}/ \
    --output ${rpm_topdir}/SOURCES/sngrep-${version}.tar.gz \
    HEAD

  rpmbuild -bs --define "_topdir ${rpm_topdir}" pkg/rpm/SPECS/sngrep.spec

  mv ${rpm_topdir}/SRPMS/* ${resultdir}
}

build_rpm() {
  local rpm_topdir=${workdir}/rpmbuild
  rpmbuild -bb --define "_topdir ${rpm_topdir}" pkg/rpm/SPECS/sngrep.spec
  find ${rpm_topdir}/RPMS/ -name "*.rpm" -exec mv {} ${resultdir} \;
}

build_apk() {
  local pkgver=$(grep -oP "(?<=^pkgver=).*" pkg/apk/APKBUILD)

  #" Comment for mcedit

  git archive \
    --format tar.gz \
    --prefix sngrep-${pkgver}/ \
    --output sngrep-${pkgver}.tar.gz \
    HEAD

  sha512sum=$(sha512sum sngrep-${pkgver}.tar.gz)
  sed -i -e "s/^sha512sums=.*/sha512sums=\"${sha512sum}\"/" pkg/apk/APKBUILD
  mv sngrep-${pkgver}.tar.gz /var/cache/distfiles/sngrep-${pkgver}.tar.gz

  echo Created dist archive sngrep-${pkgver}.tar.gz

  cd pkg/apk
  abuild -r
  # mv apk files to result dir
  find ~/packages/ -name "*.apk" -exec mv {} ${resultdir} \;
}

build_dep() {
  cp -R . ${workdir}/sngrep
  cd ${workdir}/sngrep
  ln -s pkg/debian/ .
  dpkg-buildpackage
  rm -Rf ${workdir}/sngrep
  find ${workdir} -name "*.deb" -exec mv {} ${resultdir} \;
  find ${workdir} -name "*.dsc" -exec mv {} ${resultdir} \;
}

dist=$(get_dist)

rm -Rf ${resultdir}
mkdir -p ${resultdir}
cd ${sngrep_sources}
build ${dist}

rm -Rf ${workdir}
