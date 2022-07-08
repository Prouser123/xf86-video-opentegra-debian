from debian

WORKDIR /src

COPY . .

RUN sed -i 's/# deb-src/deb-src/' /etc/apt/sources.list && \
    apt build-dep xserver-xorg-video-opentegra -y && \
    dpkg-build-package -b -uc -us