from debian

WORKDIR /work/src

COPY . .

# mk-build-deps --install | https://stackoverflow.com/a/19432425 (comments)

RUN apt-get update && \
    apt-get install devscripts -y && \
    mk-build-deps --install --tool='apt-get -o Debug::pkgProblemResolver=yes --no-install-recommends --yes' debian/control && \
    dpkg-buildpackage -b -uc -us -j $(nproc) && \
    mkdir -p /work/out && \
    cp ../*.deb /work/out