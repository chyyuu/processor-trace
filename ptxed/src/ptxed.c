/*
 * Copyright (c) 2013-2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#if defined(FEATURE_ELF)
# include "load_elf.h"
#endif /* defined(FEATURE_ELF) */

#include "pt_cpu.h"

#include "intel-pt.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

#include <xed-state.h>
#include <xed-init.h>
#include <xed-error-enum.h>
#include <xed-decode.h>
#include <xed-decoded-inst-api.h>
#include <xed-machine-mode-enum.h>


/* A collection of options. */
struct ptxed_options {
	/* Do not print the instruction. */
	uint32_t dont_print_insn:1;

	/* Remain as quiet as possible - excluding error messages. */
	uint32_t quiet:1;

	/* Print statistics (overrides quiet). */
	uint32_t print_stats:1;

	/* Print information about section loads and unloads. */
	uint32_t track_image:1;

	/* Print in AT&T format. */
	uint32_t att_format:1;

	/* Print the offset into the trace file. */
	uint32_t print_offset:1;

	/* Print the raw bytes for an insn. */
	uint32_t print_raw_insn:1;
};

/* A collection of statistics. */
struct ptxed_stats {
	/* The number of instructions. */
	uint64_t insn;
};


static void version(const char *name)
{
	struct pt_version v = pt_library_version();

	printf("%s-%d.%d.%d%s / libipt-%" PRIu8 ".%" PRIu8 ".%" PRIu32 "%s\n",
	       name, PT_VERSION_MAJOR, PT_VERSION_MINOR, PT_VERSION_BUILD,
	       PT_VERSION_EXT, v.major, v.minor, v.build, v.ext);
}

static void help(const char *name)
{
	printf("usage: %s [<options>]\n\n"
	       "options:\n"
	       "  --help|-h                     this text.\n"
	       "  --version                     display version information and exit.\n"
	       "  --att                         print instructions in att format.\n"
	       "  --no-inst                     do not print instructions (only addresses).\n"
	       "  --quiet|-q                    do not print anything (except errors).\n"
	       "  --offset                      print the offset into the trace file.\n"
	       "  --raw-insn                    print the raw bytes of each instruction.\n"
	       "  --stat                        print statistics (even when quiet).\n"
	       "  --verbose|-v                  print various information (even when quiet).\n"
	       "  --pt <file>[:<from>[-<to>]]   load the processor trace data from <file>.\n"
	       "                                an optional offset or range can be given.\n"
#if defined(FEATURE_ELF)
	       "  --elf <<file>[:<base>]        load an ELF from <file> at address <base>.\n"
	       "                                use the default load address if <base> is omitted.\n"
#endif /* defined(FEATURE_ELF) */
	       "  --raw <file>:<base>           load a raw binary from <file> at address <base>.\n"
	       "  --cpu none|auto|f/m[/s]       set cpu to the given value and decode according to:\n"
	       "                                  none     spec (default)\n"
	       "                                  auto     current cpu\n"
	       "                                  f/m[/s]  family/model[/stepping]\n"
	       "  --mtc-freq <n>                set the MTC frequency (IA32_RTIT_CTL[17:14]) to <n>.\n"
	       "  --nom-freq <n>                set the nominal frequency (MSR_PLATFORM_INFO[15:8]) to <n>.\n"
	       "  --cpuid-0x15.eax              set the value of cpuid[0x15].eax.\n"
	       "  --cpuid-0x15.ebx              set the value of cpuid[0x15].ebx.\n"
	       "\n"
#if defined(FEATURE_ELF)
	       "You must specify at least one binary or ELF file (--raw|--elf).\n"
#else /* defined(FEATURE_ELF) */
	       "You must specify at least one binary file (--raw).\n"
#endif /* defined(FEATURE_ELF) */
	       "You must specify exactly one processor trace file (--pt).\n",
	       name);
}

static int extract_base(char *arg, uint64_t *base)
{
	char *sep, *rest;

	sep = strrchr(arg, ':');
	if (sep) {
		uint64_t num;

		if (!sep[1])
			return 0;

		errno = 0;
		num = strtoull(sep+1, &rest, 0);
		if (errno || *rest)
			return 0;

		*base = num;
		*sep = 0;
		return 1;
	}

	return 0;
}

static int parse_range(const char *arg, uint64_t *begin, uint64_t *end)
{
	char *rest;

	if (!arg || !*arg)
		return 0;

	errno = 0;
	*begin = strtoull(arg, &rest, 0);
	if (errno)
		return -1;

	if (!*rest)
		return 1;

	if (*rest != '-')
		return -1;

	*end = strtoull(rest+1, &rest, 0);
	if (errno || *rest)
		return -1;

	return 2;
}

static int load_file(uint8_t **buffer, size_t *size, char *arg,
		     const char *prog)
{
	uint64_t begin_arg, end_arg;
	uint8_t *content;
	size_t read;
	FILE *file;
	long fsize, begin, end;
	int errcode, range_parts;
	char *range;

	if (!buffer || !size || !arg || !prog) {
		fprintf(stderr, "%s: internal error.\n", prog ? prog : "");
		return -1;
	}

	range_parts = 0;
	begin_arg = 0ull;
	end_arg = UINT64_MAX;

	range = strrchr(arg, ':');
	if (range) {
		/* Let's try to parse an optional range suffix.
		 *
		 * If we can, remove it from the filename argument.
		 * If we can not, assume that the ':' is part of the filename,
		 * e.g. a drive letter on Windows.
		 */
		range_parts = parse_range(range + 1, &begin_arg, &end_arg);
		if (range_parts <= 0) {
			begin_arg = 0ull;
			end_arg = UINT64_MAX;

			range_parts = 0;
		} else
			*range = 0;
	}

	errno = 0;
	file = fopen(arg, "rb");
	if (!file) {
		fprintf(stderr, "%s: failed to open %s: %d.\n",
			prog, arg, errno);
		return -1;
	}

	errcode = fseek(file, 0, SEEK_END);
	if (errcode) {
		fprintf(stderr, "%s: failed to determine size of %s: %d.\n",
			prog, arg, errno);
		goto err_file;
	}

	fsize = ftell(file);
	if (fsize < 0) {
		fprintf(stderr, "%s: failed to determine size of %s: %d.\n",
			prog, arg, errno);
		goto err_file;
	}

	/* Truncate the range to fit into the file unless an explicit range end
	 * was provided.
	 */
	if (range_parts < 2)
		end_arg = (uint64_t) fsize;

	begin = (long) begin_arg;
	end = (long) end_arg;
	if ((uint64_t) begin != begin_arg || (uint64_t) end != end_arg) {
		fprintf(stderr, "%s: invalid offset/range argument.\n", prog);
		goto err_file;
	}

	if (fsize <= begin) {
		fprintf(stderr, "%s: offset 0x%lx outside of %s.\n",
			prog, begin, arg);
		goto err_file;
	}

	if (fsize < end) {
		fprintf(stderr, "%s: range 0x%lx outside of %s.\n",
			prog, end, arg);
		goto err_file;
	}

	if (end <= begin) {
		fprintf(stderr, "%s: bad range.\n", prog);
		goto err_file;
	}

	fsize = end - begin;

	content = malloc(fsize);
	if (!content) {
		fprintf(stderr, "%s: failed to allocated memory %s.\n",
			prog, arg);
		goto err_file;
	}

	errcode = fseek(file, begin, SEEK_SET);
	if (errcode) {
		fprintf(stderr, "%s: failed to load %s: %d.\n",
			prog, arg, errno);
		goto err_content;
	}

	read = fread(content, fsize, 1, file);
	if (read != 1) {
		fprintf(stderr, "%s: failed to load %s: %d.\n",
			prog, arg, errno);
		goto err_content;
	}

	fclose(file);

	*buffer = content;
	*size = fsize;

	return 0;

err_content:
	free(content);

err_file:
	fclose(file);
	return -1;
}

static int load_pt(struct pt_config *config, char *arg, const char *prog)
{
	uint8_t *buffer;
	size_t size;
	int errcode;

	errcode = load_file(&buffer, &size, arg, prog);
	if (errcode < 0)
		return errcode;

	config->begin = buffer;
	config->end = buffer + size;

	return 0;
}

static int load_raw(struct pt_image *image, char *arg, const char *prog)
{
	uint64_t base;
	int errcode, has_base;

	has_base = extract_base(arg, &base);
	if (has_base <= 0)
		return 1;

	errcode = pt_image_add_file(image, arg, 0, UINT64_MAX, NULL, base);
	if (errcode < 0) {
		fprintf(stderr, "%s: failed to add %s at 0x%" PRIx64 ": %s.\n",
			prog, arg, base, pt_errstr(pt_errcode(errcode)));
		return 1;
	}

	return 0;
}

static xed_machine_mode_enum_t translate_mode(enum pt_exec_mode mode)
{
	switch (mode) {
	case ptem_unknown:
		return XED_MACHINE_MODE_INVALID;

	case ptem_16bit:
		return XED_MACHINE_MODE_LEGACY_16;

	case ptem_32bit:
		return XED_MACHINE_MODE_LEGACY_32;

	case ptem_64bit:
		return XED_MACHINE_MODE_LONG_64;
	}

	return XED_MACHINE_MODE_INVALID;
}

static void print_insn(const struct pt_insn *insn, xed_state_t *xed,
		       const struct ptxed_options *options, uint64_t offset)
{
	if (!insn || !options) {
		printf("[internal error]\n");
		return;
	}

	if (insn->resynced)
		printf("[overflow]\n");

	if (insn->enabled)
		printf("[enabled]\n");

	if (insn->resumed)
		printf("[resumed]\n");

	if (insn->speculative)
		printf("? ");

	if (options->print_offset)
		printf("%016" PRIx64 "  ", offset);

	printf("%016" PRIx64, insn->ip);

	if (options->print_raw_insn) {
		uint8_t i;

		printf(" ");
		for (i = 0; i < insn->size; ++i)
			printf(" %02x", insn->raw[i]);

		for (; i < sizeof(insn->raw); ++i)
			printf("   ");
	}

	if (!options->dont_print_insn) {
		xed_machine_mode_enum_t mode;
		xed_decoded_inst_t inst;
		xed_error_enum_t errcode;

		mode = translate_mode(insn->mode);

		xed_state_set_machine_mode(xed, mode);
		xed_decoded_inst_zero_set_mode(&inst, xed);

		errcode = xed_decode(&inst, insn->raw, insn->size);
		switch (errcode) {
		case XED_ERROR_NONE: {
			xed_print_info_t pi;
			char buffer[256];
			xed_bool_t ok;

			xed_init_print_info(&pi);
			pi.p = &inst;
			pi.buf = buffer;
			pi.blen = sizeof(buffer);
			pi.runtime_address = insn->ip;

			if (options->att_format)
				pi.syntax = XED_SYNTAX_ATT;

			ok = xed_format_generic(&pi);
			if (!ok) {
				printf(" [xed print error]");
				break;
			}

			printf("  %s", buffer);
		}
			break;

		default:
			printf(" [xed decode error: (%u) %s]", errcode,
			       xed_error_enum_t2str(errcode));
			break;
		}
	}

	printf("\n");

	if (insn->interrupted)
		printf("[interrupt]\n");

	if (insn->aborted)
		printf("[aborted]\n");

	if (insn->committed)
		printf("[committed]\n");

	if (insn->disabled)
		printf("[disabled]\n");

	if (insn->stopped)
		printf("[stopped]\n");
}

static void diagnose(const char *errtype, struct pt_insn_decoder *decoder,
		     const struct pt_insn *insn, int errcode)
{
	int err;
	uint64_t pos;

	err = pt_insn_get_offset(decoder, &pos);
	if (err < 0) {
		printf("could not determine offset: %s\n",
		       pt_errstr(pt_errcode(err)));
		printf("[?, %" PRIx64 ": %s: %s]\n", insn->ip, errtype,
		       pt_errstr(pt_errcode(errcode)));
	} else
		printf("[%" PRIx64 ", %" PRIx64 ": %s: %s]\n", pos,
		       insn->ip, errtype, pt_errstr(pt_errcode(errcode)));
}

static void decode(struct pt_insn_decoder *decoder,
		   const struct ptxed_options *options,
		   struct ptxed_stats *stats)
{
	xed_state_t xed;
	uint64_t offset, sync;

	if (!options) {
		printf("[internal error]\n");
		return;
	}

	xed_state_zero(&xed);

	offset = 0ull;
	sync = 0ull;
	for (;;) {
		struct pt_insn insn;
		int errcode;

		/* Initialize the IP - we use it for error reporting. */
		insn.ip = 0ull;

		errcode = pt_insn_sync_forward(decoder);
		if (errcode < 0) {
			uint64_t new_sync;

			if (errcode == -pte_eos)
				break;

			diagnose("sync error", decoder, &insn, errcode);

			/* Let's see if we made any progress.  If we haven't,
			 * we likely never will.  Bail out.
			 *
			 * We intentionally report the error twice to indicate
			 * that we tried to re-sync.  Maybe it even changed.
			 */
			errcode = pt_insn_get_offset(decoder, &new_sync);
			if (errcode < 0 || (new_sync <= sync))
				break;

			sync = new_sync;
			continue;
		}

		for (;;) {
			if (options->print_offset) {
				errcode = pt_insn_get_offset(decoder, &offset);
				if (errcode < 0)
					break;
			}

			errcode = pt_insn_next(decoder, &insn, sizeof(insn));
			if (errcode < 0) {
				/* Even in case of errors, we may have succeeded
				 * in decoding the current instruction.
				 */
				if (insn.iclass != ptic_error) {
					if (!options->quiet)
						print_insn(&insn, &xed, options,
							   offset);
					if (stats)
						stats->insn += 1;
				}
				break;
			}

			if (!options->quiet)
				print_insn(&insn, &xed, options, offset);

			if (stats)
				stats->insn += 1;

			if (errcode & pts_eos) {
				if (!insn.disabled && !options->quiet)
					printf("[end of trace]\n");

				errcode = -pte_eos;
				break;
			}
		}

		/* We shouldn't break out of the loop without an error. */
		if (!errcode)
			errcode = -pte_internal;

		/* We're done when we reach the end of the trace stream. */
		if (errcode == -pte_eos)
			break;

		diagnose("error", decoder, &insn, errcode);
	}
}

static void print_stats(struct ptxed_stats *stats)
{
	if (!stats) {
		printf("[internal error]\n");
		return;
	}

	printf("insn: %" PRIu64 ".\n", stats->insn);
}

static int get_arg_uint64(uint64_t *value, const char *option, const char *arg,
			  const char *prog)
{
	char *rest;

	if (!value || !option || !prog) {
		fprintf(stderr, "%s: internal error.\n", prog ? prog : "?");
		return 0;
	}

	if (!arg || (arg[0] == '-' && arg[1] == '-')) {
		fprintf(stderr, "%s: %s: missing argument.\n", prog, option);
		return 0;
	}

	errno = 0;
	*value = strtoull(arg, &rest, 0);
	if (errno || *rest) {
		fprintf(stderr, "%s: %s: bad argument: %s.\n", prog, option,
			arg);
		return 0;
	}

	return 1;
}

static int get_arg_uint32(uint32_t *value, const char *option, const char *arg,
			  const char *prog)
{
	uint64_t val;

	if (!get_arg_uint64(&val, option, arg, prog))
		return 0;

	if (val > UINT32_MAX) {
		fprintf(stderr, "%s: %s: value too big: %s.\n", prog, option,
			arg);
		return 0;
	}

	*value = (uint32_t) val;

	return 1;
}

static int get_arg_uint8(uint8_t *value, const char *option, const char *arg,
			 const char *prog)
{
	uint64_t val;

	if (!get_arg_uint64(&val, option, arg, prog))
		return 0;

	if (val > UINT8_MAX) {
		fprintf(stderr, "%s: %s: value too big: %s.\n", prog, option,
			arg);
		return 0;
	}

	*value = (uint8_t) val;

	return 1;
}

extern int main(int argc, char *argv[])
{
	struct pt_insn_decoder *decoder;
	struct ptxed_options options;
	struct ptxed_stats stats;
	struct pt_config config;
	struct pt_image *image;
	const char *prog;
	int errcode, i;

	if (!argc) {
		help("");
		return 1;
	}

	prog = argv[0];
	decoder = NULL;

	memset(&options, 0, sizeof(options));
	memset(&stats, 0, sizeof(stats));

	pt_config_init(&config);

	image = pt_image_alloc(NULL);
	if (!image) {
		fprintf(stderr, "%s: failed to allocate image.\n", prog);
		return 1;
	}

	for (i = 1; i < argc;) {
		char *arg;

		arg = argv[i++];

		if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
			help(prog);
			goto out;
		}
		if (strcmp(arg, "--version") == 0) {
			version(prog);
			goto out;
		}
		if (strcmp(arg, "--pt") == 0) {
			if (argc <= i) {
				fprintf(stderr,
					"%s: --pt: missing argument.\n", prog);
				goto out;
			}
			arg = argv[i++];

			if (decoder) {
				fprintf(stderr,
					"%s: duplicate pt sources: %s.\n",
					prog, arg);
				goto err;
			}

			errcode = pt_cpu_errata(&config.errata, &config.cpu);
			if (errcode < 0)
				goto err;

			errcode = load_pt(&config, arg, prog);
			if (errcode < 0)
				goto err;

			decoder = pt_insn_alloc_decoder(&config);
			if (!decoder) {
				fprintf(stderr,
					"%s: failed to create decoder.\n",
					prog);
				goto err;
			}

			errcode = pt_insn_set_image(decoder, image);
			if (errcode < 0) {
				fprintf(stderr,
					"%s: failed to set image.\n",
					prog);
				goto err;
			}

			continue;
		}
		if (strcmp(arg, "--raw") == 0) {
			if (argc <= i) {
				fprintf(stderr,
					"%s: --raw: missing argument.\n", prog);
				goto out;
			}
			arg = argv[i++];

			errcode = load_raw(image, arg, prog);
			if (errcode < 0)
				goto err;

			continue;
		}
#if defined(FEATURE_ELF)
		if (strcmp(arg, "--elf") == 0) {
			uint64_t base;

			if (argc <= i) {
				fprintf(stderr,
					"%s: --elf: missing argument.\n", prog);
				goto out;
			}
			arg = argv[i++];
			base = 0ull;
			errcode = extract_base(arg, &base);
			if (errcode < 0)
				goto err;

			errcode = load_elf(image, arg, base, prog,
					   options.track_image);
			if (errcode < 0)
				goto err;

			continue;
		}
#endif /* defined(FEATURE_ELF) */
		if (strcmp(arg, "--att") == 0) {
			options.att_format = 1;
			continue;
		}
		if (strcmp(arg, "--no-inst") == 0) {
			options.dont_print_insn = 1;
			continue;
		}
		if (strcmp(arg, "--quiet") == 0 || strcmp(arg, "-q") == 0) {
			options.quiet = 1;
			continue;
		}
		if (strcmp(arg, "--offset") == 0) {
			options.print_offset = 1;
			continue;
		}
		if (strcmp(arg, "--raw-insn") == 0) {
			options.print_raw_insn = 1;
			continue;
		}
		if (strcmp(arg, "--stat") == 0) {
			options.print_stats = 1;
			continue;
		}
		if (strcmp(arg, "--cpu") == 0) {
			/* override cpu information before the decoder
			 * is initialized.
			 */
			if (decoder) {
				fprintf(stderr,
					"%s: please specify cpu before the pt source file.\n",
					prog);
				goto err;
			}
			if (argc <= i) {
				fprintf(stderr,
					"%s: --cpu: missing argument.\n", prog);
				goto out;
			}
			arg = argv[i++];

			if (strcmp(arg, "auto") == 0) {
				errcode = pt_cpu_read(&config.cpu);
				if (errcode < 0) {
					fprintf(stderr,
						"%s: error reading cpu: %s.\n",
						prog,
						pt_errstr(pt_errcode(errcode)));
					return 1;
				}
				continue;
			}

			if (strcmp(arg, "none") == 0) {
				memset(&config.cpu, 0, sizeof(config.cpu));
				continue;
			}

			errcode = pt_cpu_parse(&config.cpu, arg);
			if (errcode < 0) {
				fprintf(stderr,
					"%s: cpu must be specified as f/m[/s]\n",
					prog);
				goto err;
			}
			continue;
		}
		if (strcmp(arg, "--mtc-freq") == 0) {
			if (!get_arg_uint8(&config.mtc_freq, "--mtc-freq",
					   argv[i++], prog))
				goto err;

			continue;
		}
		if (strcmp(arg, "--nom-freq") == 0) {
			if (!get_arg_uint8(&config.nom_freq, "--nom-freq",
					   argv[i++], prog))
				goto err;

			continue;
		}
		if (strcmp(arg, "--cpuid-0x15.eax") == 0) {
			if (!get_arg_uint32(&config.cpuid_0x15_eax,
					    "--cpuid-0x15.eax", argv[i++],
					    prog))
				goto err;

			continue;
		}
		if (strcmp(arg, "--cpuid-0x15.ebx") == 0) {
			if (!get_arg_uint32(&config.cpuid_0x15_ebx,
					    "--cpuid-0x15.ebx", argv[i++],
					    prog))
				goto err;

			continue;
		}
		if (strcmp(arg, "--verbose") == 0 || strcmp(arg, "-v") == 0) {
			options.track_image = 1;
			continue;
		}

		fprintf(stderr, "%s: unknown option: %s.\n", prog, arg);
		goto err;
	}

	if (!decoder) {
		fprintf(stderr, "%s: no pt file.\n", prog);
		goto err;
	}

	xed_tables_init();
	decode(decoder, &options, &stats);

	if (options.print_stats)
		print_stats(&stats);

out:
	pt_insn_free_decoder(decoder);
	pt_image_free(image);
	return 0;

err:
	pt_insn_free_decoder(decoder);
	pt_image_free(image);
	return 1;
}
