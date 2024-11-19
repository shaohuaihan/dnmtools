/* xcounts: reformat counts so they only give the m and u counts in a
 * dynamic step wig format
 *
 * Copyright (C) 2023 Andrew D. Smith
 *
 * Authors: Andrew D. Smith
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <bamxx.hpp>

#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

// from smithlab_cpp
#include "OptionParser.hpp"
#include "smithlab_os.hpp"
#include "smithlab_utils.hpp"

#include "MSite.hpp"
#include "counts_header.hpp"
#include "dnmt_error.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::runtime_error;
using std::string;
using std::to_chars;
using std::to_string;
using std::vector;

using bamxx::bgzf_file;

template <typename T>
static inline uint32_t
fill_output_buffer(const uint32_t offset, const MSite &s, T &buf) {
  auto buf_end = buf.data() + buf.size();
  auto res = to_chars(buf.data(), buf_end, s.pos - offset);
  *res.ptr++ = '\t';
  res = to_chars(res.ptr, buf_end, s.n_meth());
  *res.ptr++ = '\t';
  res = to_chars(res.ptr, buf_end, s.n_unmeth());
  *res.ptr++ = '\n';
  return std::distance(buf.data(), res.ptr);
}

int
main_xcounts(int argc, const char **argv) {
  try {
    bool verbose = false;
    bool gzip_output = false;
    bool require_coverage = false;
    size_t n_threads = 1;
    string genome_file;
    string header_file;

    string outfile{"-"};
    const string description =
      "compress counts files by removing context information";

    /****************** COMMAND LINE OPTIONS ********************/
    OptionParser opt_parse(strip_path(argv[0]), description,
                           "<counts-file> (\"-\" for standard input)", 1);
    opt_parse.add_opt("output", 'o', "output file (default is standard out)",
                      false, outfile);
    opt_parse.add_opt("chroms", 'c', "make header from this reference", false,
                      genome_file);
    opt_parse.add_opt("reads", 'r', "ouput only sites with reads", false,
                      require_coverage);
    opt_parse.add_opt("header", 'h', "use this file to generate header", false,
                      header_file);
    opt_parse.add_opt("threads", 't', "threads for compression (use few)",
                      false, n_threads);
    opt_parse.add_opt("zip", 'z',
                      "gzip compress output (automatic if input is gzip)",
                      false, gzip_output);
    opt_parse.add_opt("verbose", 'v', "print more run info", false, verbose);
    std::vector<string> leftover_args;
    opt_parse.parse(argc, argv, leftover_args);
    if (argc == 1 || opt_parse.help_requested()) {
      cerr << opt_parse.help_message() << endl
           << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.about_requested()) {
      cerr << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.option_missing()) {
      cerr << opt_parse.option_missing_message() << endl;
      return EXIT_SUCCESS;
    }
    if (leftover_args.size() != 1) {
      cerr << opt_parse.help_message() << endl;
      return EXIT_SUCCESS;
    }
    const string filename(leftover_args.front());
    /****************** END COMMAND LINE OPTIONS *****************/

    vector<string> chrom_names;
    vector<uint64_t> chrom_sizes;
    if (!genome_file.empty()) {
      const int ret = get_chrom_sizes_for_counts_header(
        n_threads, genome_file, chrom_names, chrom_sizes);
      if (ret)
        throw dnmt_error{"failed to get chrom sizes from: " + genome_file};
    }

    bamxx::bam_tpool tpool(n_threads);
    bgzf_file in(filename, "r");
    if (!in)
      throw dnmt_error{"could not open file: " + filename};

    const auto outfile_mode = (gzip_output || in.is_compressed()) ? "w" : "wu";

    bgzf_file out(outfile, outfile_mode);
    if (!out)
      throw dnmt_error{"error opening output file: " + outfile};

    if (n_threads > 1) {
      if (in.is_bgzf())
        tpool.set_io(in);
      tpool.set_io(out);
    }

    if (!header_file.empty())
      write_counts_header_from_file(header_file, out);
    else if (!genome_file.empty())
      write_counts_header_from_chrom_sizes(chrom_names, chrom_sizes, out);

    // use the kstring_t type to more directly use the BGZF file
    kstring_t line{0, 0, nullptr};
    const int ret = ks_resize(&line, 1024);
    if (ret)
      throw dnmt_error("failed to acquire buffer");

    vector<char> buf(128);

    uint32_t offset = 0;
    string prev_chrom;
    bool status_ok = true;
    bool found_header = (!genome_file.empty() || !header_file.empty());

    MSite site;
    while (status_ok && bamxx::getline(in, line)) {
      if (is_counts_header_line(line.s)) {
        if (!genome_file.empty() || !header_file.empty())
          continue;
        found_header = true;
        const string header_line{line.s};
        write_counts_header_line(header_line, out);
        continue;
      }

      status_ok = site.initialize(line.s, line.s + line.l);
      if (!status_ok || !found_header)
        break;

      if (site.chrom != prev_chrom) {
        if (verbose)
          cerr << "processing: " << site.chrom << endl;
        prev_chrom = site.chrom;
        offset = 0;

        site.chrom += '\n';
        const int64_t sz = size(site.chrom);
        status_ok = bgzf_write(out.f, site.chrom.data(), sz) == sz;
      }
      if (site.n_reads > 0) {
        const int64_t sz = fill_output_buffer(offset, site, buf);
        status_ok = bgzf_write(out.f, buf.data(), sz) == sz;
        offset = site.pos;
      }
    }
    ks_free(&line);

    if (!status_ok) {
      cerr << "failed converting " << filename << " to " << outfile << endl;
      return EXIT_FAILURE;
    }
    if (!found_header) {
      cerr << "no header provided or found" << endl;
      return EXIT_FAILURE;
    }
  }
  catch (const std::exception &e) {
    cerr << e.what() << endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
