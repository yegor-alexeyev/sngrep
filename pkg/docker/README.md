To build image for RHEL-7, RHEL-8 need define variables `repo_owner`, `base_image`, `image_tag`, `rhel_username`, `rhel_password` and then start build image like

```
export repo_owner=irontec
export base_image=rhel
export image_tag=8
export rhel_username=${your_username}
export rhel_password=${your_password}
docker build \
    --no-cache \
    --build-arg base_image=registry.redhat.io/ubi${image_tag} \
    --build-arg image_tag=latest \
    --build-arg rhel_username=${rhel_username} \
    --build-arg rhel_password=${rhel_password} \
    -t ${repo_owner}/sngrep-builder:${base_image}-${image_tag} \
    -f Dockerfile-builder .
```

To build image for OpenSUSEneed define variables `repo_owner`, `base_image`, `image_tag` and then start build image like

```
export repo_owner=irontec
export base_image=opensuse/leap
export image_tag=15
docker build \
    --no-cache \
    --build-arg base_image=registry.redhat.io/ubi${image_tag} \
    --build-arg image_tag=latest \
    --build-arg rhel_username=${rhel_username} \
    --build-arg rhel_password=${rhel_password} \
    -t ${repo_owner}/sngrep-builder:opensuse-${image_tag} \
    -f Dockerfile-builder .
```

For other dist to build image need to define environement variables `repo_owner`, `base_image`, `image_tag` and then start build image like

```
export repo_owner=irontec
export base_image=fedora
export image_tag=35
docker build \
    --no-cache \
    --build-arg base_image=${base_image} \
    --build-arg image_tag=${image_tag} \
    -t ${repo_owner}/sngrep-builder:${base_image}-${image_tag} \
    -f Dockerfile-builder .
```

Suported dist

| dist                | version |
|---------------------|---------|
| debian              | 10      |
| debian              | 9       |
| rhel                | 8       |
| rhel                | 7       |
| centos              | 8       |
| centos              | 7       |
| fedora              | 35      |
| fedora              | 34      |
| fedora              | 33      |
| fedora              | 32      |
| alpine              | 3.13    |
| alpine              | 3.12    |
| alpine              | 3.11    |
| opensuse/leap       | 15      |
| opensuse/tumbleweed | latest  |

How to use builder image

```
mkdir context
cd context
git clone https://github.com/irontec/sngrep.git
docker run --rm -it \
  -v `pwd`:/context \
  ${repo_owner}/sngrep-builder:${base_image}-${image_tag}
```
