version 1.0

import "vcf_line_splitter.wdl" as tasks

workflow test {
    input {
        String test_vcf_url = "https://1000genomes.s3.amazonaws.com/release/20130502/ALL.chr22.phase3_shapeit2_mvncall_integrated_v5a.20130502.genotypes.vcf.gz"
        String docker = "vcf_line_splitter"
    }

    call aria2c {
        input:
            url = test_vcf_url
    }

    call tasks.vcf_line_splitter {
        input:
            vcf_gz = aria2c.file,
            docker = docker,
            partMB = 64
    }

    call validate {
        input:
            vcf_gz = aria2c.file,
            parts_vcf_gz = vcf_line_splitter.parts_vcf_gz
    }
}

task aria2c {
    input {
        String url
        Int connections = 10
    }
 
    command <<<
        set -euxo pipefail
        mkdir __out
        cd __out
        aria2c -x ~{connections} -j ~{connections} -s ~{connections} --file-allocation=none --retry-wait=2 "~{url}"
    >>>

    output {
        File file = glob("__out/*")[0]
    }

    runtime {
        docker: "hobbsau/aria2"
    }
}

task validate {
    input {
        File vcf_gz
        Array[File] parts_vcf_gz
    }

    command <<<
        set -eux
        export LC_ALL=C
        cp "~{write_lines(parts_vcf_gz)}" parts_manifest

        gzip -dc "~{vcf_gz}" | sha256sum | cut -f1 -d ' ' > lhs & pid=$!

        gzip -dc "~{parts_vcf_gz[0]}" | head -c 1048576 | grep ^\# > header.txt
        (cat header.txt; sort parts_manifest | xargs -n 999999 gzip -dc | grep -v ^\#) | sha256sum | cut -f1 -d ' ' > rhs

        wait $pid
        if [[ "$(cat lhs)" != "$(cat rhs)" ]]; then
            exit 1
        fi
    >>>

    output {
        String input_digest = read_string("lhs")
        String output_digest = read_string("rhs")
    }

    runtime {
        docker: "ubuntu:19.04"
        cpu: 8
    }
}
