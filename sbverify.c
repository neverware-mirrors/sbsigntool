/*
 * Copyright (C) 2012 Jeremy Kerr <jeremy.kerr@canonical.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <getopt.h>

#include <ccan/talloc/talloc.h>
#include <ccan/read_write_all/read_write_all.h>

#include "image.h"
#include "idc.h"

#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pkcs7.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

static const char *toolname = "sbverify";

enum verify_status {
	VERIFY_FAIL = 0,
	VERIFY_OK = 1,
};

static struct option options[] = {
	{ "cert", required_argument, NULL, 'c' },
	{ "no-verify", no_argument, NULL, 'n' },
	{ "detached", required_argument, NULL, 'd' },
	{ "help", no_argument, NULL, 'h' },
	{ "version", no_argument, NULL, 'V' },
	{ NULL, 0, NULL, 0 },
};

static void usage(void)
{
	printf("Usage: %s [options] --cert <certfile> <efi-boot-image>\n"
		"Verify a UEFI secure boot image.\n\n"
		"Options:\n"
		"\t--cert <certfile>  certificate (x509 certificate)\n"
		"\t--no-verify        don't perform certificate verification\n"
		"\t--detached <file>  read signature from <file>, instead of\n"
		"\t                    looking for an embedded signature\n",
			toolname);
}

static void version(void)
{
	printf("%s %s\n", toolname, VERSION);
}

int load_cert(X509_STORE *certs, const char *filename)
{
	X509 *cert;
	BIO *bio;

	bio = NULL;
	cert = NULL;

	bio = BIO_new_file(filename, "r");
	if (!bio) {
		fprintf(stderr, "Couldn't open file %s\n", filename);
		goto err;
	}

	cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
	if (!cert) {
		fprintf(stderr, "Couldn't read certificate file %s\n",
				filename);
		goto err;
	}

	X509_STORE_add_cert(certs, cert);
	return 0;

err:
	ERR_print_errors_fp(stderr);
	if (cert)
		X509_free(cert);
	if (bio)
		BIO_free(bio);
	return -1;
}

static int load_image_signature_data(struct image *image,
		uint8_t **buf, size_t *len)
{
	struct cert_table_header *header;

	if (!image->data_dir_sigtable->addr
			|| !image->data_dir_sigtable->size) {
		fprintf(stderr, "No signature table present\n");
		return -1;
	}

	header = image->buf + image->data_dir_sigtable->addr;
	*buf = (void *)(header + 1);
	*len = header->size;
	return 0;
}

static int load_detached_signature_data(struct image *image,
		const char *filename, uint8_t **buf, size_t *len)
{
	struct stat statbuf;
	uint8_t *tmpbuf = NULL;
	int fd, rc;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Couldn't open %s: %s\n", filename,
				strerror(errno));
		return -1;
	}

	rc = fstat(fd, &statbuf);
	if (rc) {
		perror("stat");
		goto err;
	}

	tmpbuf = talloc_array(image, uint8_t, statbuf.st_size);
	if (!tmpbuf) {
		perror("talloc_array");
		goto err;
	}

	rc = read_all(fd, tmpbuf, statbuf.st_size);
	if (!rc) {
		perror("read_all");
		goto err;
	}

	*buf = tmpbuf;
	*len = statbuf.st_size;
	return 0;

err:
	close(fd);
	talloc_free(tmpbuf);
	return -1;
}

static int x509_verify_cb(int status, X509_STORE_CTX *ctx)
{
	int err = X509_STORE_CTX_get_error(ctx);

	/* also accept code-signing keys */
	if (err == X509_V_ERR_INVALID_PURPOSE
			&& ctx->cert->ex_xkusage == XKU_CODE_SIGN)
		status = 1;

	return status;
}

int main(int argc, char **argv)
{
	const char *detached_sig_filename, *image_filename;
	enum verify_status status;
	int rc, c, flags, verify;
	const uint8_t *tmp_buf;
	struct image *image;
	X509_STORE *certs;
	uint8_t *sig_buf;
	size_t sig_size;
	struct idc *idc;
	BIO *idcbio;
	PKCS7 *p7;

	status = VERIFY_FAIL;
	certs = X509_STORE_new();
	verify = 1;
	detached_sig_filename = NULL;

	OpenSSL_add_all_digests();
	ERR_load_crypto_strings();

	for (;;) {
		int idx;
		c = getopt_long(argc, argv, "c:d:nVh", options, &idx);
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			rc = load_cert(certs, optarg);
			if (rc)
				return EXIT_FAILURE;
			break;
		case 'd':
			detached_sig_filename = optarg;
			break;
		case 'n':
			verify = 0;
			break;
		case 'V':
			version();
			return EXIT_SUCCESS;
		case 'h':
			usage();
			return EXIT_SUCCESS;
		}

	}

	if (argc != optind + 1) {
		usage();
		return EXIT_FAILURE;
	}

	image_filename = argv[optind];

	image = image_load(image_filename);
	if (!image) {
		fprintf(stderr, "Can't open image %s\n", image_filename);
		return EXIT_FAILURE;
	}

	image_find_regions(image);

	if (detached_sig_filename)
		rc = load_detached_signature_data(image, detached_sig_filename,
				&sig_buf, &sig_size);
	else
		rc = load_image_signature_data(image, &sig_buf, &sig_size);

	if (rc) {
		fprintf(stderr, "Unable to read signature data from %s\n",
				detached_sig_filename ? : image_filename);
		goto out;
	}

	tmp_buf = sig_buf;
	p7 = d2i_PKCS7(NULL, &tmp_buf, sig_size);
	if (!p7) {
		fprintf(stderr, "Unable to parse signature data\n");
		ERR_print_errors_fp(stderr);
		goto out;
	}

	idcbio = BIO_new(BIO_s_mem());
	idc = IDC_get(p7, idcbio);
	if (!idc)
		goto out;

	rc = IDC_check_hash(idc, image);
	if (rc)
		goto out;

	flags = PKCS7_BINARY;
	if (!verify)
		flags |= PKCS7_NOVERIFY;

	X509_STORE_set_verify_cb_func(certs, x509_verify_cb);
	rc = PKCS7_verify(p7, NULL, certs, idcbio, NULL, flags);
	if (!rc) {
		printf("PKCS7 verification failed\n");
		ERR_print_errors_fp(stderr);
		goto out;
	}

	status = VERIFY_OK;

out:
	talloc_free(image);
	if (status == VERIFY_OK)
		printf("Signature verification OK\n");
	else
		printf("Signature verification failed\n");

	return status == VERIFY_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
