from debian

WORKDIR /src

COPY . .

RUN apt build-dep xserver-xorg-video-opentegra -y && \
    dpkg-build-package -b -uc -us