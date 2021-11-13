/*
vcf_line_splitter: split up a VCF file (streamed uncompressed through stdin) into parts consisting
of contiguous blocks of lines from the original file, written to bgzf temporary files. The
header is repeated at the top of each part. The temporary filenames are written to stdout.
*/

#include <iostream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <chrono>
#include <unistd.h>
#include <string.h>
#include <gflags/gflags.h>
#include <htslib/bgzf.h>

using namespace std;

// Helper class for streaming lines from an istream (with peek capability)
class end_of_file : public exception {};
class linepeeker {
    istream& _input;
    string _next;
    bool _full;
    unsigned int _count;
    size_t _last_size;

public:
    linepeeker(istream& input)
        : _input(input), _full(false), _count(0), _last_size(0) {
        _input.exceptions(istream::badbit);
    }

    const string& peek() {
        if (!_full) {
            // Preallocate buffer based on _last_size to reduce realloaction during getline
            _next.reserve(_last_size * 5 / 4);

            // Read a line
            if (!getline(_input, _next)) {
                if (_input.eof()) throw end_of_file();
                throw runtime_error("input failure");
            }
            _full = true;

            _last_size = _next.size();
        }

        return _next;
    }

    void drop() {
        _full = false;
    }

    // Like peek();drop(); but uses move semantics to avoid copying
    string get() {
        peek();
        _full = false;
        return move(_next);
    }
};

// read in the VCF header
string read_header(linepeeker& input) {
    try {
        stringstream buf;
        string line;
        while (true) {
            line = input.peek();
            if (line.length() < 1 || line[0] != '#') {
                break;
            }
            buf << line << endl;
            input.drop();
        }
        return buf.str();
    } catch (const end_of_file& err) {
        throw runtime_error("Premature EOF while reading VCF header");
    }
}

// command-line flags
DEFINE_int32(lines, 1000000, "lines per part");
DEFINE_int32(MB, 0, "megabytes per part, before compression; overrides -lines");
DEFINE_int32(threads, 1, "max compress+flush background threads");
DEFINE_bool(part_column, false, "print an extra column with the part number on standard output");
DEFINE_bool(quiet, false, "don't print any extra info to standard error");
DEFINE_string(range, "", "chr:beg-end; include only lines with CHROM:POS within this inclusive range");

// coordination globals
mutex master_lock;
int threads_launched, threads_completed, threads_active;
std::condition_variable cv_thread_exit;
double read_s, stall_s, write_s;
size_t records_read, records_written, bytes_processed, records_skipped;

// background thread to compress and write out a part
void writer_thread(const string& dest_prefix, const int part_num, const string& header, unique_ptr<vector<string>> buf) {
    stringstream pathbuf;
    pathbuf << dest_prefix << setfill('0') << setw(6) << part_num << ".vcf.gz";
    string path = pathbuf.str();
    size_t ct = 0, sz = 0;

    auto t0 = chrono::system_clock::now();
    BGZF* ogz = bgzf_open(path.c_str(), "wbx1");

    if (!ogz || bgzf_write(ogz, header.c_str(), header.size()) != header.size() || bgzf_flush(ogz)) {
        throw runtime_error("Error opening part file " + path + " for writing; delete if it already exists");
    }
    sz += header.size();

    for (auto it = buf->begin(); it != buf->end(); it++) {
        if (bgzf_write(ogz, it->c_str(), it->size()) != it->size() || bgzf_write(ogz, "\n", 1) != 1) {
            throw runtime_error("Error writing VCF data");
        }
        sz += it->size() + 1; ct++;
        std::string().swap(*it);  // free memory asap
    }

    if (bgzf_close(ogz)) {
        throw runtime_error("Error closing VCF output");
    }
    buf->clear();

    stringstream outbuf;
    outbuf << path;
    if (FLAGS_part_column) {
        outbuf << "\t" << setfill('0') << setw(6) << part_num;
    }
    outbuf << endl;
    string outstr = outbuf.str();

    {
        lock_guard<mutex> lock(master_lock);
        // using low-level write to avoid cout sync issues
        if (write(1,outstr.c_str(),outstr.size()) != (int)outstr.size()) {
            throw runtime_error("Error writing filename to standard output");
        }
        if (!FLAGS_quiet) {
            cerr << "vcf_line_splitter wrote " << path
                 << " (" << sz/1048576 << " MB before compression)"
                 << endl;
        }
        --threads_active; ++threads_completed;
        write_s += chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now() - t0).count()/1000.0;
        records_written += ct; bytes_processed += sz;
        cv_thread_exit.notify_all();
    }
}

bool part_finished(int lines, size_t bytes) {
    return (FLAGS_MB > 0)
               ? (bytes >= size_t(FLAGS_MB)*1048576)
               : (lines >= FLAGS_lines);
}

bool in_range(const string& line, string range_chrom, unsigned long long range_beg, unsigned long long range_end) {
    if (range_chrom.empty()) {
        return true;
    }
    auto p = line.find('\t');
    if (p == range_chrom.size()) {
        if (!strncmp(range_chrom.c_str(), line.c_str(), p)) {
            auto p2 = line.find('\t', p+1);
            if (p2 != string::npos && p2 > p+1) {
                errno = 0;
                auto pos = strtoull(line.substr(p+1, p2-p-1).c_str(), nullptr, 10);
                if (errno == 0 && pos >= range_beg && pos <= range_end) {
                    return true;
                }
            }
        }
    }
    return false;
}

// read in and launch background processing of one part
bool part(const string& dest_prefix, const int part_num, const string& header, linepeeker& input,
          string range_chrom, unsigned long long range_beg, unsigned long long range_end) {
    unique_ptr<vector<string>> buf(new vector<string>);
    unsigned int lines = 0;
    size_t bytes = 0;
    bool more = true;
    auto t0 = chrono::system_clock::now();

    // read part
    try {
        while (!part_finished(lines,bytes)) {
            string line = input.get();
            if (!in_range(line, range_chrom, range_beg, range_end)) {
                ++records_skipped;
                continue;
            }
            lines++; bytes += line.size() + 1;
            buf->push_back(move(line));
        }
    } catch(const end_of_file& ok) {
        more = false;
    }

    if (lines) {
        unique_lock<mutex> lock(master_lock);
        records_read += lines;
        auto t1 = chrono::system_clock::now();
        read_s += chrono::duration_cast<chrono::milliseconds>(t1 - t0).count()/1000.0;
        // wait for thread slot availability
        while (threads_active >= FLAGS_threads) {
            cv_thread_exit.wait(lock);
        }
        // launch worker thread
        ++threads_launched;
        thread th(writer_thread,dest_prefix,part_num,header,move(buf));
        if (!th.joinable()) {
            throw runtime_error("couldn't launch worker thread");
        }
        th.detach();
        ++threads_active;
        stall_s += chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now() - t1).count()/1000.0;
    }

    return more;
}

int main(int argc, char **argv) {
    using namespace google;
    SetUsageMessage("Split up a VCF file streamed through standard input\n\
Usage: bgzip -dc@ 4 | vcf_line_splitter -threads $(nproc) /destination/path/prefix");
    ParseCommandLineFlags(&argc, &argv, true);
    if (argc < 2 || isatty(STDIN_FILENO)) {
        ShowUsageWithFlags(argv[0]);
        return 1;
    }

    // parse --range if any
    string range_chrom;
    unsigned long long range_beg = -1, range_end = -1;
    if (!FLAGS_range.empty()) {
        errno = 0;
        auto pc = FLAGS_range.find(':');
        if (pc != string::npos && pc > 0) {
            range_chrom = FLAGS_range.substr(0, pc);
            auto pd = FLAGS_range.find('-', pc);
            if (pd != string::npos && pd > pc+1) {
                range_beg = strtoull(FLAGS_range.substr(pc+1, pd-pc-1).c_str(), nullptr, 10);
                range_end = strtoull(FLAGS_range.substr(pd+1).c_str(), nullptr, 10);
            }
        }
        if (range_chrom.empty() || range_beg <= 0 || range_end < range_beg || errno) {
            cerr << "Unable to parse --range as chr:beg-end" << endl;
            cerr.flush();
            return -1;
        }
    }

    ios::sync_with_stdio(false); // cin performance is awful without this!

    string dest_prefix(argv[1]);
    linepeeker input(cin);
    string hdr = read_header(input);
    threads_launched = threads_completed = threads_active = 0;
    read_s = stall_s = write_s = 0.0;
    records_read = records_written = bytes_processed = 0;

    int part_num = 0;
    while(part(dest_prefix, part_num++, hdr, input, range_chrom, range_beg, range_end));

    // wait for running threads
    unique_lock<mutex> lock(master_lock);
    while (threads_active) {
        cv_thread_exit.wait(lock);
    }

    if (threads_launched != threads_completed || records_read != records_written) {
        cerr << "BUG!!!" << endl;
        cerr.flush();
        return -1;
    }

    if (!FLAGS_quiet) {
        cerr << "vcf_line_splitter wrote " << part_num << " parts with "
             << records_read << " records and " << bytes_processed<< " uncompressed bytes";
        if (records_skipped) {
            cerr << " (" << records_skipped << " records range-skipped)";
        }
        cerr << "; spent " << (int) read_s << "s reading and " << (int) write_s << "s writing"
             << ", " << (int) stall_s << "s stalled"
             << endl;
    }

    return 0;
}
