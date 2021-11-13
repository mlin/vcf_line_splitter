version 1.0

import "vcf_line_splitter.wdl" as tasks

workflow test {
    input {
        File? test_vcf_gz
        String test_vcf_url = "https://1000genomes.s3.amazonaws.com/release/20130502/ALL.chr22.phase3_shapeit2_mvncall_integrated_v5a.20130502.genotypes.vcf.gz"
        String docker = "vcf_line_splitter"
    }

    if (!defined(test_vcf_gz)) {
        call aria2c {
            input:
                url = test_vcf_url
        }
    }

    File eff_test_vcf_gz = select_first([test_vcf_gz, aria2c.file])

    call tasks.vcf_line_splitter {
        input:
            vcf_gz = eff_test_vcf_gz,
            docker = docker,
            partMB = 256, cpu = 4
    }

    call validate {
        input:
            vcf_gz = eff_test_vcf_gz,
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
        aria2c -x ~{connections} -j ~{connections} -s ~{connections} \
            --file-allocation=none --retry-wait=2 --stderr=true \
            "~{url}"
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

        # digest the uncompressed contents of vcf_gz
        gzip -dc "~{vcf_gz}" | sha256sum > input_digest & pid=$!

        # "unsplit" the parts and digest them
        gzip -dc "~{parts_vcf_gz[0]}" | head -c 1048576 | grep ^\# > header.txt
        cp "~{write_lines(parts_vcf_gz)}" manifest
        (cat header.txt;
            sort manifest | xargs -n 999999 gzip -dc | grep -v ^\#) \
            | sha256sum > output_digest

        # compare the digests
        wait $pid
        diff input_digest output_digest

        # check all the headers too
        while read partfn; do
            gzip -dc "$partfn" | head -c 1048756 | grep ^\# > header2.txt
            diff header.txt header2.txt
        done < manifest
    >>>

    output {
        String input_digest = read_string("input_digest")
        String output_digest = read_string("output_digest")
    }

    runtime {
        docker: "ubuntu:20.04"
        cpu: 8
    }
}
