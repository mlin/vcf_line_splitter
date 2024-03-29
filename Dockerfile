FROM ubuntu:20.04 as builder
RUN apt-get -qq update && apt-get -y -qq install \
    g++ \
    libgflags-dev \
    libhts-dev \
    libjemalloc-dev \
    make \
    zlib1g-dev
WORKDIR /vcf_line_splitter
ADD . .
RUN make

FROM ubuntu:20.04
RUN apt-get -qq update && apt-get -y -qq install \
    tabix \
    libgflags2.2 \
    libhts3 \
    libjemalloc2 \
    pv \
    zlib1g
COPY --from=builder /vcf_line_splitter/build/vcf_line_splitter /usr/local/bin
CMD ["vcf_line_splitter"]
