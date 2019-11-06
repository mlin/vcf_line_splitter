FROM ubuntu:19.10 as builder
RUN apt-get -qq update && apt-get -y -qq install \
    g++ \
    libgflags-dev \
    libhts-dev \
    libjemalloc-dev \
    make
WORKDIR /vcf_line_splitter
ADD . .
RUN make

FROM ubuntu:19.10
RUN apt-get -qq update && apt-get -y -qq install \
    tabix \
    libgflags2.2 \
    libhts2 \
    libjemalloc2
COPY --from=builder /vcf_line_splitter/build/vcf_line_splitter /usr/local/bin
CMD ["vcf_line_splitter"]
