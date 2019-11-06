version 1.0

task vcf_line_splitter {
    input {
        File vcf_gz
        Int partMB = 1024
        String parts_name = basename(vcf_gz, ".vcf.gz")

        Int cpu = 16
        String memory = "16G"
        String docker = "quay.io/mlin/vcf_line_splitter"
    }

    command <<<
        set -euo pipefail
        mkdir parts
        bgzip -dc@ 4 "~{vcf_gz}" | vcf_line_splitter -MB ~{partMB} -threads ~{cpu} "parts/~{parts_name}."
    >>>

    output {
        Array[File] parts_vcf_gz = glob("parts/*.vcf.gz")
    }

    runtime {
        cpu: cpu
        memory: memory
        docker: docker
    }
}
