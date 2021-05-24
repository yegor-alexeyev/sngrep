#!/bin/bash

set -o errexit -o nounset -o pipefail

# Uncomment for debug output
#set -x

get_dist() {
  grep -oP  "(?<=^ID=).*" /etc/os-release | sed -e 's/"//g'
}

get_dist_version() {
  grep -oP  "(?<=^VERSION_ID=).*" /etc/os-release | sed -e 's/"//g'
}

install_env() {
  local dist=$1
  local dist_version=$2
  case ${dist} in
    rhel)
      subscription-manager register --username="${rhel_username}" --password="${rhel_password}"
      subscription-manager attach
      if [[ "${dist_version}" =~ "7" ]]; then
        yum update -y
        yum install -y yum-utils git rpm-build
        yum-config-manager --enable rhel-7-server-optional-rpms
      elif [[ "${dist_version}" =~ "8" ]]; then
        dnf update -y
        dnf install -y 'dnf-command(builddep)' git rpm-build
        dnf config-manager --set-enabled codeready-builder-for-rhel-8-x86_64-rpms
      else
          echo "not supported dist version: ${dist_version}"
          exit 1
      fi
      adduser builder
      ;;
    centos)
      yum install -y dnf
      dnf update -y
      dnf install -y 'dnf-command(builddep)' git rpm-build
      if [ "${dist_version}" -eq 8 ]; then
        dnf config-manager --set-enabled powertools
      fi
      adduser builder
      ;;
    fedora)
      dnf update -y
      dnf install -y 'dnf-command(builddep)' git rpm-build
      adduser builder
      ;;
    debian)
      apt-get update
      apt-get -y upgrade
      apt-get install -y build-essential devscripts git
      adduser builder
      ;;
    alpine)
      apk --no-cache upgrade
      apk --no-cache add alpine-sdk sudo git
      adduser -D builder
      addgroup builder abuild
      echo "%abuilder ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/abuild
      su - builder -c "git config --global user.name 'Your Full Name'"
      su - builder -c "git config --global user.email 'your@email.address'"
      su - builder -c "abuild-keygen -a -i"
      ;;
    opensuse-leap)
      zypper -n update
      zypper -n install curl git rpm-build
      useradd builder
      ;;
    opensuse-tumbleweed)
      zypper -n update
      zypper -n install curl shadow git rpm-build
      useradd builder
      ;;
  esac
}

install_deps() {
  local dist=$1
  local dist_version=$2
  case ${dist} in
    rhel)
      curl -s -S -O https://raw.githubusercontent.com/irontec/sngrep/master/pkg/rpm/SPECS/sngrep.spec
      if [[ "${dist_version}" =~ "7" ]]; then
        yum-builddep -y sngrep.spec
      else
        dnf builddep -y sngrep.spec
      fi
      rm -f sngrep.spec
      ;;
    centos)
      curl -s -S -O https://raw.githubusercontent.com/irontec/sngrep/master/pkg/rpm/SPECS/sngrep.spec
      dnf builddep -y sngrep.spec
      rm -f sngrep.spec
      ;;
    fedora)
      dnf builddep -y https://raw.githubusercontent.com/irontec/sngrep/master/pkg/rpm/SPECS/sngrep.spec
      ;;
    debian)
      git clone https://github.com/irontec/sngrep.git
      cd sngrep
      ln -s pkg/debian/ .
      dpkg-source -b .
      cd ..
      rm -Rf sngrep
      mk-build-deps -i --tool="apt-get --no-install-recommends -y" sngrep_*.dsc
      rm -f /sngrep*
      ;;
    alpine)
      local TMPDIR=$(mktemp -d)
      curl -s -S -o ${TMPDIR}/APKBUILD https://raw.githubusercontent.com/alpinelinux/aports/master/community/sngrep/APKBUILD
      chown -R builder ${TMPDIR}
      su - builder -c "cd ${TMPDIR}; abuild deps"
      rm -Rf ${TMPDIR}
      ;;
    opensuse-leap)
      curl -s -S -O https://raw.githubusercontent.com/irontec/sngrep/master/pkg/rpm/SPECS/sngrep.spec
      zypper -n install $(rpmspec -P sngrep.spec | grep BuildRequires | sed -r -e 's/BuildRequires:\s+//' -e 's/,//g' | xargs)
      rm -f sngrep.spec
      ;;
    opensuse-tumbleweed)
      curl -s -S -O https://raw.githubusercontent.com/irontec/sngrep/master/pkg/rpm/SPECS/sngrep.spec
      zypper -n install $(rpmspec -P sngrep.spec | grep BuildRequires | sed -r -e 's/BuildRequires:\s+//' -e 's/,//g' | xargs)
      rm -f sngrep.spec
      ;;
  esac
}

cleanup() {
  local dist=$1
  local dist_version=$2
  case ${dist} in
    rhel)
      yum clean all
      if [[ "${dist_version}" =~ "8" ]]; then
        dnf clean all
      fi
      subscription-manager remove --all
      subscription-manager unregister
      ;;
    centos)
      yum clean all
      dnf clean all
      ;;
    fedora)
      dnf clean all
      ;;
    debian)
      apt-get clean
      ;;
    opensuse-leap)
      zypper clean --all
      ;;
    opensuse-tumbleweed)
      zypper clean --all
      ;;
  esac
}

echo "Preparing sngrep builder using '${base_image}:${image_tag}' image"; \
dist=$(get_dist)
dist_version=$(get_dist_version)

install_env ${dist} ${dist_version}
install_deps ${dist} ${dist_version}
cleanup ${dist} ${dist_version}

