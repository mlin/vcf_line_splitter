# vcf_line_splitter

### Split a huge VCF file into multiple parts, quickly

Starting with `big.vcf.gz`,

```
bgzip -dc@ 4 big.vcf.gz | vcf_line_splitter -MB 1024 -threads $(nproc) small-
```

Writes `small-000000.vcf.gz`, `small-000001.vcf.gz`, `small-000002.vcf.gz`, ..., each including the header and roughly one gigabyte worth (before compression) of the variant lines from the original VCF (contiguous and in-order).

It's multithreaded C++ code to do this at high speed. Memory usage is liable to scale as the specified part size, times the number of threads, with a healthy overhead factor.

Compile with `make`, dependencies listed in the [Dockerfile](https://github.com/mlin/vcf_line_splitter/blob/master/Dockerfile).

### Motivation

Modern cohort sequencing projects now produce joint .vcf.gz files that are individually hundreds of gigabytes or more, compressed. Parallel analytics environments like Apache Spark are powerful ways to work with such datasets, but importing individual files of that size can still be terribly slow and inefficient, because they're initially processed in just one thread. Splitting the file beforehand lets us parallelize the import (e.g. with a glob argument to `SparkContext.textFile`) which is much better.

So we wrote this utility with custom multithreading, and other annoying low-level tricks, to split up the lines of a big VCF file into recompressed partitions as quickly as possible, to prepare for import to Spark or similar.

Alternatives and their problems:

* coreutils `split`: materializes uncompressed data on disk (too big), or blocks main thread on recompression (too slow); doesn't copy the header into each part.
* `tabix`: complications when variants span the edges of genome regions extracted; have to had run single-threaded tabix indexing.

### Limitations

Splitting occurs between VCF lines, but otherwise without regard to their genome positions or content. Therefore, a part may contain variants on many contigs, or the variants on one contig may be found in many parts. Similarly, groups of related variants (e.g. by phase set, or spanning deletion) may be split across parts.

### Maximizing throughput

Besides adding more threads, make sure you have new versions of bgzip and htslib which support multithreaded BGZF and use [libdeflate](https://github.com/ebiggers/libdeflate). To verify, `ldd $(which bgzip) vcf_line_splitter` and check that both use `libdeflate.so`.

### Docker & WDL

The published Docker image has all the dependencies on board, and might be used like so:

```
docker run --rm -it -v $(pwd):/work quay.io/mlin/vcf_line_splitter bash -euo pipefail \
    "bgzip -dc@ 4 /work/big.vcf.gz | vcf_line_splitter -MB 1024 -threads $(nproc) /work/small-"
```

We've also included a [WDL task](https://github.com/mlin/vcf_line_splitter/blob/master/vcf_line_splitter.wdl) definition.
