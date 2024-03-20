#*
#*  GRUB  --  GRand Unified Bootloader
#*  Copyright (C) 2009  Free Software Foundation, Inc.
#*
#*  GRUB is free software: you can redistribute it and/or modify
#*  it under the terms of the GNU General Public License as published by
#*  the Free Software Foundation, either version 3 of the License, or
#*  (at your option) any later version.
#*
#*  GRUB is distributed in the hope that it will be useful,
#*  but WITHOUT ANY WARRANTY; without even the implied warranty of
#*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#*  GNU General Public License for more details.
#*
#*  You should have received a copy of the GNU General Public License
#*  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
#*

import re
import sys
import os
import datetime
import codecs

if len (sys.argv) < 3:
    print ("Usage: %s SOURCE DESTINATION" % sys.argv[0])
    exit (0)
indir = sys.argv[1]
outdir = sys.argv[2]

basedir = os.path.join (outdir, "lib/libgcrypt-grub")
try:
    os.makedirs (basedir)
except:
    print ("WARNING: %s already exists" % basedir)
cipher_dir_in = os.path.join (indir, "cipher")
cipher_dir_out = os.path.join (basedir, "cipher")
try:
    os.makedirs (cipher_dir_out)
except:
    print ("WARNING: %s already exists" % cipher_dir_out)
mpidir =  os.path.join (basedir, "mpi")
try:
    os.makedirs (mpidir)
except:
    print ("WARNING: %s already exists" % mpidir)

srcdir =  os.path.join (basedir, "src")
try:
    os.makedirs (srcdir)
except:
    print ("WARNING: %s already exists" % srcdir)

cipher_files = sorted (os.listdir (cipher_dir_in))
conf = codecs.open (os.path.join ("grub-core", "Makefile.gcry.def"), "w", "utf-8")
conf.write ("AutoGen definitions Makefile.tpl;\n\n")
confutil = codecs.open ("Makefile.utilgcry.def", "w", "utf-8")
confutil.write ("AutoGen definitions Makefile.tpl;\n\n")
confutil.write ("library = {\n");
confutil.write ("  name = libgrubgcry.a;\n");
confutil.write ("  cflags = '$(CFLAGS_GCRY)';\n");
confutil.write ("  cppflags = '$(CPPFLAGS_GCRY)';\n");
confutil.write ("  extra_dist = grub-core/lib/libgcrypt-grub/cipher/ChangeLog;\n");
confutil.write ("\n");
chlog = ""
modules_sym_md = []

# Strictly speaking CRC32/CRC24 work on bytes so this value should be 1
# But libgcrypt uses 64. Let's keep the value for compatibility. Since
# noone uses CRC24/CRC32 for HMAC this is no problem
mdblocksizes = {"_gcry_digest_spec_crc32" : 64,
                "_gcry_digest_spec_crc32_rfc1510" : 64,
                "_gcry_digest_spec_crc24_rfc2440" : 64,
                "_gcry_digest_spec_md4" : 64,
                "_gcry_digest_spec_md5" : 64,
                "_gcry_digest_spec_rmd160" : 64,
                "_gcry_digest_spec_sha1" : 64,
                "_gcry_digest_spec_sha224" : 64,
                "_gcry_digest_spec_sha256" : 64,
                "_gcry_digest_spec_sha384" : 128,
                "_gcry_digest_spec_sha512" : 128,
                "_gcry_digest_spec_sha512_256": 128,
                "_gcry_digest_spec_sha512_224": 128,
                "_gcry_digest_spec_tiger" : 64,
                "_gcry_digest_spec_tiger1" : 64,
                "_gcry_digest_spec_tiger2" : 64,
                "_gcry_digest_spec_whirlpool" : 64,
                "_gcry_digest_spec_shake128": 64,
                "_gcry_digest_spec_shake256": 64,
                "_gcry_digest_spec_sm3": 64,
                "_gcry_digest_spec_stribog_256": 64,
                "_gcry_digest_spec_stribog_512": 64,
                "_gcry_digest_spec_sha3_224": 1152 / 8,
                "_gcry_digest_spec_sha3_256": 1088 / 8,
                "_gcry_digest_spec_sha3_384": 832 / 8,
                "_gcry_digest_spec_sha3_512": 576 / 8,
                "_gcry_digest_spec_gost3411_94": 32,
                "_gcry_digest_spec_gost3411_cp": 32}

cryptolist = codecs.open (os.path.join (cipher_dir_out, "crypto.lst"), "w", "utf-8")

# rijndael is the only cipher using aliases. So no need for mangling, just
# hardcode it
cryptolist.write ("RIJNDAEL: gcry_rijndael\n");
cryptolist.write ("RIJNDAEL192: gcry_rijndael\n");
cryptolist.write ("RIJNDAEL256: gcry_rijndael\n");
cryptolist.write ("AES128: gcry_rijndael\n");
cryptolist.write ("AES-128: gcry_rijndael\n");
cryptolist.write ("AES-192: gcry_rijndael\n");
cryptolist.write ("AES-256: gcry_rijndael\n");

cryptolist.write ("ADLER32: adler32\n");
cryptolist.write ("CRC64: crc64\n");

extra_files = {
    "gcry_camellia": ["camellia.c"], # Main file is camellia-glue.c
    "gcry_sha512"  : ["hash-common.c"],
} 
extra_files_list = [x for xs in extra_files.values() for x in xs]

for cipher_file in cipher_files:
    infile = os.path.join (cipher_dir_in, cipher_file)
    outfile = os.path.join (cipher_dir_out, cipher_file)
    if cipher_file == "ChangeLog" or cipher_file == "ChangeLog-2011":
        continue
    chlognew = "	* %s" % cipher_file
    # Unused generic support files
    if re.match ("(Makefile\.am|md\.c|cipher\.c|cipher-.*\.c|mac-.*\.c|mac\.c|pubkey\.c|pubkey-util\.c)$", cipher_file):
        chlog = "%s%s: Removed\n" % (chlog, chlognew)
        continue
    # TODO: Support KDF
    if re.match ("(kdf\.c|scrypt\.c)$", cipher_file):
        chlog = "%s%s: Removed\n" % (chlog, chlognew)
        continue
    # TODO: Use optimized versions
    if re.match ("(.*\.[sS]|.*-intel-shaext\.c|.*-ssse3-i386\.c|.*-ppc\.c|.*-ssse3-amd64\.c|.*-s390x\.c|rijndael-aesni\.c|crc-intel-pclmul\.c|.*-armv8-ce\.c|.*-p10le\.c|rijndael-padlock.c|rijndael-ppc9le.c|rijndael-vaes.c)$", cipher_file):
        chlog = "%s%s: Removed\n" % (chlog, chlognew)
        continue
    # Autogenerated files. Not even worth mentionning in ChangeLog
    if re.match ("Makefile\.in$", cipher_file):
        continue
    nch = False
    if re.match (".*\.[ch]$", cipher_file):
        isc = re.match (".*\.c$", cipher_file)
        f = codecs.open (infile, "r", "utf-8")
        fw = codecs.open (outfile, "w", "utf-8")
        fw.write ("/* This file was automatically imported with \n")
        fw.write ("   import_gcry.py. Please don't modify it */\n")
        fw.write ("#include <grub/dl.h>\n")
        if cipher_file == "camellia.h":
            fw.write ("#include <grub/misc.h>\n")
            fw.write ("void camellia_setup128(const unsigned char *key, grub_uint32_t *subkey);\n")
            fw.write ("void camellia_setup192(const unsigned char *key, grub_uint32_t *subkey);\n")
            fw.write ("void camellia_setup256(const unsigned char *key, grub_uint32_t *subkey);\n")
            fw.write ("void camellia_encrypt128(const grub_uint32_t *subkey, grub_uint32_t *io);\n")
            fw.write ("void camellia_encrypt192(const grub_uint32_t *subkey, grub_uint32_t *io);\n")                      
            fw.write ("void camellia_encrypt256(const grub_uint32_t *subkey, grub_uint32_t *io);\n")                      
            fw.write ("void camellia_decrypt128(const grub_uint32_t *subkey, grub_uint32_t *io);\n")
            fw.write ("void camellia_decrypt192(const grub_uint32_t *subkey, grub_uint32_t *io);\n")                      
            fw.write ("void camellia_decrypt256(const grub_uint32_t *subkey, grub_uint32_t *io);\n")                      
            fw.write ("#define memcpy grub_memcpy\n")
        # Whole libgcrypt is distributed under GPLv3+ or compatible
        if isc:
            fw.write ("GRUB_MOD_LICENSE (\"GPLv3+\");\n")

        ciphernames = []
        mdnames = []
        mdctxsizes = []
        pknames = []
        hold = False
        skip = 0
        skip2 = False
        ismd = False
        mdarg = 0
        ispk = False
        iscipher = False
        iscryptostart = False
        iscomma = False
        skip_statement = False
        skip_comma = False
        if isc:
            modname = "gcry_%s" % cipher_file.removesuffix(".c").removesuffix("-glue").replace("-", "_")
        for line in f:
            line = line
            if skip_statement:
                if not re.search (";", line) is None:
                    skip_statement = False
                continue
            if skip > 0:
                if line[0] == "}":
                    skip = skip - 1
                continue
            if skip2:
                if not re.search (" *};", line) is None:
                    skip2 = False
                continue
            if iscryptostart:
                s = re.search (" *\"([A-Z0-9_a-z-]*)\"", line)
                if not s is None:
                    sg = s.groups()[0]
                    cryptolist.write (("%s: %s\n") % (sg, modname))
                    iscryptostart = False
            if ismd:
                spl = line.split (",")
                if mdarg + len (spl) > 9 and mdarg <= 9 and ("sizeof" in spl[9-mdarg]):
                    mdctxsizes.append (spl[9-mdarg].lstrip ().rstrip())
                mdarg = mdarg + len (spl) - 1
            if ismd or iscipher or ispk:
                if not re.search (" *};", line) is None:
                    if not iscomma:
                        fw.write ("    ,\n")
                    fw.write ("#ifdef GRUB_UTIL\n");
                    fw.write ("    .modname = \"%s\",\n" % modname);
                    fw.write ("#endif\n");
                    if ismd:
                        if not (mdname in mdblocksizes):
                            print ("ERROR: Unknown digest blocksize: %s\n"
                                   % mdname)
                            exit (1)
                        fw.write ("    .blocksize = %s\n"
                                  % mdblocksizes [mdname])
                    ismd = False
                    mdarg = 0
                    iscipher = False
                    ispk = False
                iscomma = not re.search (",$", line) is None
            # Used only for selftests.
            m = re.match ("(static byte|static unsigned char) (weak_keys_chksum)\[[0-9]*\] =", line)
            if not m is None:
                skip = 1
                fname = m.groups ()[1]
                chmsg = "(%s): Removed." % fname
                if nch:
                    chlognew = "%s\n	%s" % (chlognew, chmsg)
                else:
                    chlognew = "%s %s" % (chlognew, chmsg)
                    nch = True
                continue
            if (not hold) and (re.match (r"[ \t]*(run_selftests|do_tripledes_set_extra_info),?", line) is not None):
                iscomma = True
                line = ""
            if hold:
                hold = False
                # We're optimising for size and exclude anything needing good
                # randomness.
                if re.match ("(_gcry_hash_selftest_check_one|bulk_selftest_setkey|run_selftests|do_tripledes_set_extra_info|selftest|sm4_selftest|_gcry_[a-z0-9_]*_hash_buffer|tripledes_set2keys|_gcry_rmd160_mixblock|serpent_test|dsa_generate_ext|test_keys|gen_k|sign|gen_x931_parm_xp|generate_x931|generate_key|dsa_generate|dsa_sign|ecc_sign|generate|generate_fips186|_gcry_register_pk_dsa_progress|_gcry_register_pk_ecc_progress|progress|scanval|ec2os|ecc_generate_ext|ecc_generate|compute_keygrip|ecc_get_param|_gcry_register_pk_dsa_progress|gen_x931_parm_xp|gen_x931_parm_xi|rsa_decrypt|rsa_sign|rsa_generate_ext|rsa_generate|secret|check_exponent|rsa_blind|rsa_unblind|extract_a_from_sexp|curve_free|curve_copy|point_set)", line) is not None:

                    skip = 1
                    if not re.match ("selftest", line) is None and cipher_file == "idea.c":
                        skip = 3

                    if not re.match ("serpent_test", line) is None:
                        fw.write ("static const char *serpent_test (void) { return 0; }\n");
                    if not re.match ("sm4_selftest", line) is None:
                        fw.write ("static const char *sm4_selftest (void) { return 0; }\n");
                    hash_buf = re.match ("(_gcry_[a-z0-9_]*_hash_buffers)", line)
                    if hash_buf is not None:
                        fw.write ("#define %s 0" % (hash_buf.group(0)))
                    if not re.match ("dsa_generate", line) is None:
                        fw.write ("#define dsa_generate 0");
                    if not re.match ("ecc_generate", line) is None:
                        fw.write ("#define ecc_generate 0");
                    if not re.match ("rsa_generate ", line) is None:
                        fw.write ("#define rsa_generate 0");
                    if not re.match ("rsa_sign", line) is None:
                        fw.write ("#define rsa_sign 0");
                    if not re.match ("rsa_decrypt", line) is None:
                        fw.write ("#define rsa_decrypt 0");
                    if not re.match ("dsa_sign", line) is None:
                        fw.write ("#define dsa_sign 0");
                    if not re.match ("ecc_sign", line) is None:
                        fw.write ("#define ecc_sign 0");
                    fname = re.match ("[a-zA-Z0-9_]*", line).group ()
                    chmsg = "(%s): Removed." % fname
                    if nch:
                        chlognew = "%s\n	%s" % (chlognew, chmsg)
                    else:
                        chlognew = "%s %s" % (chlognew, chmsg)
                        nch = True                        
                    continue
                else:
                    fw.write (holdline)
            m = re.match ("# *include <(.*)>", line)
            if not m is None:
                chmsg = "Removed including of %s" % m.groups ()[0]
                if nch:
                    chlognew = "%s\n	%s" % (chlognew, chmsg)
                else:
                    chlognew = "%s: %s" % (chlognew, chmsg)
                    nch = True
                continue
            m = re.match ("(const )?gcry_cipher_spec_t", line)
            if isc and not m is None:
                assert (not ismd)
                assert (not ispk)
                assert (not iscipher)
                assert (not iscryptostart)
                ciphername = line.removeprefix("const ").removeprefix("gcry_cipher_spec_t").strip ()
                ciphername = re.match("[a-zA-Z0-9_]*",ciphername).group ()
                ciphernames.append (ciphername)
                iscipher = True
                iscryptostart = True

            m = re.match ("(const )?gcry_pk_spec_t", line)
            if isc and not m is None:
                assert (not ismd)
                assert (not ispk)
                assert (not iscipher)
                assert (not iscryptostart)
                pkname = line.removeprefix("const ").removeprefix("gcry_pk_spec_t").strip ()
                pkname = re.match("[a-zA-Z0-9_]*",pkname).group ()
                pknames.append (pkname)
                ispk = True
                iscryptostart = True

            m = re.match ("(const )?gcry_md_spec_t", line)
            if isc and not m is None:
                assert (not ismd)
                assert (not ispk)
                assert (not iscipher)
                assert (not iscryptostart)
                line = line.removeprefix("const ")
                mdname = line.removeprefix("const ").removeprefix("gcry_md_spec_t").strip ()
                mdname = re.match("[a-zA-Z0-9_]*",mdname).group ()
                mdnames.append (mdname)
                ismd = True
                mdarg = 0
                iscryptostart = True
            m = re.match ("static const char \*selftest.*;$", line)
            if not m is None:
                fname = line[len ("static const char \*"):]
                fname = re.match ("[a-zA-Z0-9_]*", fname).group ()
                chmsg = "(%s): Removed declaration." % fname
                if nch:
                    chlognew = "%s\n	%s" % (chlognew, chmsg)
                else:
                    chlognew = "%s %s" % (chlognew, chmsg)
                    nch = True
                continue
            m = re.match ("static gcry_mpi_t gen_k .*;$", line)
            if not m is None:
                chmsg = "(gen_k): Removed declaration."
                if nch:
                    chlognew = "%s\n	%s" % (chlognew, chmsg)
                else:
                    chlognew = "%s %s" % (chlognew, chmsg)
                    nch = True
                continue
            m = re.match ("static (int|void) test_keys .*;$", line)
            if not m is None:
                chmsg = "(test_keys): Removed declaration."
                if nch:
                    chlognew = "%s\n	%s" % (chlognew, chmsg)
                else:
                    chlognew = "%s %s" % (chlognew, chmsg)
                    nch = True
                continue
            m = re.match ("static void secret .*;$", line)
            if not m is None:
                chmsg = "(secret): Removed declaration."
                if nch:
                    chlognew = "%s\n	%s" % (chlognew, chmsg)
                else:
                    chlognew = "%s %s" % (chlognew, chmsg)
                    nch = True
                continue
            m = re.match ("static void \(\*progress_cb\).*;$", line)
            if not m is None:
                chmsg = "(progress_cb): Removed declaration."
                if nch:
                    chlognew = "%s\n	%s" % (chlognew, chmsg)
                else:
                    chlognew = "%s %s" % (chlognew, chmsg)
                    nch = True
                continue
            m = re.match ("static void \*progress_cb_data.*;$", line)
            if not m is None:
                chmsg = "(progress_cb): Removed declaration."
                if nch:
                    chlognew = "%s\n	%s" % (chlognew, chmsg)
                else:
                    chlognew = "%s %s" % (chlognew, chmsg)
                    nch = True
                continue

            m = re.match ("((static )?const char( |)\*|static gpg_err_code_t|void|static int|static gcry_err_code_t|static gcry_mpi_t|static void|void|static elliptic_curve_t) *$", line)
            if not m is None:
                hold = True
                holdline = line
                continue
            m = re.match ("static int tripledes_set2keys \(.*\);", line)
            if not m is None:
                continue
            m = re.match ("static int tripledes_set3keys \(.*\);", line)
            if not m is None:
                continue
            m = re.match ("static int tripledes_set2keys \(", line)
            if not m is None:
                skip_statement = True
                continue
            m = re.match ("static int tripledes_set3keys \(", line)
            if not m is None:
                skip_statement = True
                continue
            m = re.match ("static const char sample_secret_key", line)
            if not m is None:
                skip_statement = True
                continue
            m = re.match ("static const char sample_public_key", line)
            if not m is None:
                skip_statement = True
                continue
            m = re.match ("static void sign|static gpg_err_code_t sign|static gpg_err_code_t generate",
                          line)
            if not m is None:
                skip_statement = True
                continue

            m = re.match ("cipher_extra_spec_t", line)
            if isc and not m is None:
                skip2 = True
                fname = line[len ("cipher_extra_spec_t "):]
                fname = re.match ("[a-zA-Z0-9_]*", fname).group ()
                chmsg = "(%s): Removed." % fname
                if nch:
                    chlognew = "%s\n	%s" % (chlognew, chmsg)
                else:
                    chlognew = "%s %s" % (chlognew, chmsg)
                    nch = True
                continue
            m = re.match ("pk_extra_spec_t", line)
            if isc and not m is None:
                skip2 = True
                fname = line[len ("pk_extra_spec_t "):]
                fname = re.match ("[a-zA-Z0-9_]*", fname).group ()
                chmsg = "(%s): Removed." % fname
                if nch:
                    chlognew = "%s\n	%s" % (chlognew, chmsg)
                else:
                    chlognew = "%s %s" % (chlognew, chmsg)
                    nch = True
                continue
            m = re.match ("md_extra_spec_t", line)
            if isc and not m is None:
                skip2 = True
                fname = line[len ("md_extra_spec_t "):]
                fname = re.match ("[a-zA-Z0-9_]*", fname).group ()
                chmsg = "(%s): Removed." % fname
                if nch:
                    chlognew = "%s\n	%s" % (chlognew, chmsg)
                else:
                    chlognew = "%s %s" % (chlognew, chmsg)
                    nch = True
                continue
            fw.write (line)
        if len (ciphernames) > 0 or len (mdnames) > 0 or len (pknames) > 0:
            modfiles = [cipher_file]
            if modname in extra_files.keys():
                modfiles += extra_files[modname]
            if len (ciphernames) > 0 or len (mdnames) > 0:
                modules_sym_md.append (modname)
            chmsg = "(GRUB_MOD_INIT(%s)): New function\n" % modname
            if nch:
                chlognew = "%s\n	%s" % (chlognew, chmsg)
            else:
                chlognew = "%s%s" % (chlognew, chmsg)
                nch = True
            fw.write ("\n\nGRUB_MOD_INIT(%s)\n" % modname)
            fw.write ("{\n")
            for ciphername in ciphernames:
                chmsg = "Register cipher %s" % ciphername
                chlognew = "%s\n	%s" % (chlognew, chmsg)
                fw.write ("  grub_cipher_register (&%s);\n" % ciphername)
            for ctxsize in mdctxsizes:
                fw.write ("  COMPILE_TIME_ASSERT(%s <= GRUB_CRYPTO_MAX_MD_CONTEXT_SIZE);\n" % ctxsize)
            for mdname in mdnames:
                chmsg = "Register digest %s" % mdname
                chlognew = "%s\n	%s" % (chlognew, chmsg)
                fw.write ("  grub_md_register (&%s);\n" % mdname)
            for pkname in pknames:
                chmsg = "Register pk %s" % pkname
                chlognew = "%s\n	%s" % (chlognew, chmsg)
                fw.write ("  grub_crypto_pk_%s = &%s;\n"
                          % (pkname.replace ("_gcry_pubkey_spec_", ""), pkname))
            fw.write ("}")
            chmsg = "(GRUB_MOD_FINI(%s)): New function\n" % modname
            chlognew = "%s\n	%s" % (chlognew, chmsg)
            fw.write ("\n\nGRUB_MOD_FINI(%s)\n" % modname)
            fw.write ("{\n")
            for ciphername in ciphernames:
                chmsg = "Unregister cipher %s" % ciphername
                chlognew = "%s\n	%s" % (chlognew, chmsg)
                fw.write ("  grub_cipher_unregister (&%s);\n" % ciphername)
            for mdname in mdnames:
                chmsg = "Unregister MD %s" % mdname
                chlognew = "%s\n	%s" % (chlognew, chmsg)
                fw.write ("  grub_md_unregister (&%s);\n" % mdname)
            for pkname in pknames:
                chmsg = "Unregister pk %s" % pkname
                chlognew = "%s\n	%s" % (chlognew, chmsg)
                fw.write ("  grub_crypto_pk_%s = 0;\n"
                          % (pkname.replace ("_gcry_pubkey_spec_", "")))
            fw.write ("}\n")
            conf.write ("module = {\n")
            conf.write ("  name = %s;\n" % modname)
            for src in modfiles:
                conf.write ("  common = lib/libgcrypt-grub/cipher/%s;\n" % src)
                if len (ciphernames) > 0 or len (mdnames) > 0:
                    confutil.write ("  common = grub-core/lib/libgcrypt-grub/cipher/%s;\n" % src)
            if modname == "gcry_ecc":
                conf.write ("  common = lib/libgcrypt-grub/mpi/ec.c;\n")
                conf.write ("  cflags = '$(CFLAGS_GCRY) -Wno-redundant-decls -Wno-sign-compare';\n")
            elif modname == "gcry_rijndael" or modname == "gcry_md4" or modname == "gcry_md5" or modname == "gcry_rmd160" or modname == "gcry_sha1" or modname == "gcry_sha256" or modname == "gcry_sha512" or modname == "gcry_tiger":
                # Alignment checked by hand
                conf.write ("  cflags = '$(CFLAGS_GCRY) -Wno-cast-align';\n");
            else:
                conf.write ("  cflags = '$(CFLAGS_GCRY)';\n");
            conf.write ("  cppflags = '$(CPPFLAGS_GCRY)';\n");
            conf.write ("};\n\n")
            f.close ()
            fw.close ()
            if nch:
                chlog = "%s%s\n" % (chlog, chlognew)
        elif isc and cipher_file not in extra_files_list:
            print ("WARNING: C file isn't a module: %s" % cipher_file)
            f.close ()
            fw.close ()
            os.remove (outfile)
            chlog = "%s\n	* %s: Removed" % (chlog, cipher_file)
        continue
    chlog = "%s%sSkipped unknown file\n" % (chlog, chlognew)
    print ("WARNING: unknown file %s" % cipher_file)

cryptolist.close ()

for src in sorted (os.listdir (os.path.join (indir, "src"))):
    if src == "versioninfo.rc.in" or src == "ath.c" or src == "ChangeLog-2011" \
            or src == "dumpsexp.c" or src == "fips.c" or src == "gcrypt.h.in" \
            or src == "gcryptrnd.c"or src == "getrandom.c" \
            or src == "global.c" or src == "hmac256.c" \
            or src == "hwfeatures.c" or src == "libgcrypt-config.in" \
            or src == "libgcrypt.def" or src == "libgcrypt.m4" \
            or src == "libgcrypt.vers" or src == "Makefile.am" \
            or src == "Manifest" or src == "misc.c" \
            or src == "missing-string.c" or src == "module.c" \
            or src == "secmem.c" or src == "sexp.c" \
            or src == "stdmem.c" or src == "visibility.c":
        continue
    outfile = os.path.join (basedir, "src", src)
    infile = os.path.join (indir, "src", src)
    if os.path.isdir (infile):
        continue
    fw = codecs.open (outfile, "w", "utf-8")
    if src == "gcrypt-module.h":
        fw.close ()
        continue
    if src == "visibility.h":
        fw.write ("# include <grub/gcrypt/gcrypt.h>\n")
        fw.close ()
        continue
    f = codecs.open (infile, "r", "utf-8")
    if src == "types.h":
        fw.write (f.read ().replace ("float f;", "").replace ("double g;", ""))
        f.close ()
        fw.close ()
        continue

    if src == "cipher-proto.h":
        fw.write("#include <grub/crypto.h>\n")
        fw.write("typedef gcry_selftest_func_t selftest_func_t;")
        f.close ()
        fw.close ()
        continue

    if src == "g10lib.h":
        fw.write (f.read ().replace ("(printf,f,a)", "(__printf__,f,a)").replace ("#include \"../compat/libcompat.h\"", "").replace("#define N_(a) (a)", ""))
        f.close ()
        fw.close ()
        continue

    fw.write (f.read ())
    f.close ()
    fw.close ()

for src in sorted (os.listdir (os.path.join (indir, "mpi"))):
    if src == "config.links" or src == "ChangeLog-2011" \
            or src == "Manifest" \
            or src == "Makefile.am":
        continue
    infile = os.path.join (indir, "mpi", src)
    outfile = os.path.join (basedir, "mpi", src)
    if os.path.isdir (infile):
        continue
    f = codecs.open (infile, "r", "utf-8")
    fw = codecs.open (outfile, "w", "utf-8")
    fw.write ("/* This file was automatically imported with \n")
    fw.write ("   import_gcry.py. Please don't modify it */\n")
    hold = False
    skip = 0
    for line in f:
        if skip > 0:
            if line[0] == "}":
                skip = skip - 1
            continue
        if hold:
            hold = False
            # We're optimising for size and exclude anything needing good
            # randomness.
            if not re.match ("(_gcry_mpi_get_hw_config|gcry_mpi_randomize|_gcry_mpi_randomize)", line) is None:
                skip = 1
                continue
            else:
                fw.write (holdline)
        m = re.match ("(const char( |)\*|void) *$", line)
        if not m is None:
            hold = True
            holdline = line
            continue
        m = re.match ("#include \"mod-source-info\.h\"", line)
        if not m is None:
            continue
        fw.write (line)

chlog = "%s	* crypto.lst: New file.\n" % chlog

outfile = os.path.join (cipher_dir_out, "types.h")
fw=codecs.open (outfile, "w", "utf-8")
fw.write ("#include <grub/types.h>\n")
fw.write ("#include <cipher_wrap.h>\n")
chlog = "%s	* types.h: New file.\n" % chlog
fw.close ()

outfile = os.path.join (cipher_dir_out, "memory.h")
fw=codecs.open (outfile, "w", "utf-8")
fw.write ("#include <cipher_wrap.h>\n")
chlog = "%s	* memory.h: New file.\n" % chlog
fw.close ()


outfile = os.path.join (cipher_dir_out, "cipher.h")
fw=codecs.open (outfile, "w", "utf-8")
fw.write ("#include <grub/crypto.h>\n")
fw.write ("#include <cipher_wrap.h>\n")
chlog = "%s	* cipher.h: Likewise.\n" % chlog
fw.close ()

outfile = os.path.join (cipher_dir_out, "g10lib.h")
fw=codecs.open (outfile, "w", "utf-8")
fw.write ("#include <cipher_wrap.h>\n")
chlog = "%s	* g10lib.h: Likewise.\n" % chlog
fw.close ()

conf.close ();

initfile = codecs.open (os.path.join (cipher_dir_out, "init.c"), "w", "utf-8")
initfile.write ("#include <grub/crypto.h>\n")
for module in modules_sym_md:
    initfile.write ("extern void grub_%s_init (void);\n" % module)
    initfile.write ("extern void grub_%s_fini (void);\n" % module)
initfile.write ("\n")
initfile.write ("void\n")
initfile.write ("grub_gcry_init_all (void)\n")
initfile.write ("{\n")
for module in modules_sym_md:
    initfile.write ("  grub_%s_init ();\n" % module)
initfile.write ("}\n")
initfile.write ("\n")
initfile.write ("void\n")
initfile.write ("grub_gcry_fini_all (void)\n")
initfile.write ("{\n")
for module in modules_sym_md:
    initfile.write ("  grub_%s_fini ();\n" % module)
initfile.write ("}\n")
initfile.close ()

confutil.write ("  common = grub-core/lib/libgcrypt-grub/cipher/init.c;\n")
confutil.write ("};\n");
confutil.close ();


outfile = os.path.join (cipher_dir_out, "ChangeLog")
fw=codecs.open (outfile, "w", "utf-8")
dt = datetime.date.today ()
fw.write ("%04d-%02d-%02d  Automatic import tool\n" % \
          (dt.year,dt.month, dt.day))
fw.write ("\n")
fw.write ("	Imported ciphers to GRUB\n")
fw.write ("\n")
fw.write (chlog)
fw.write ("\n")
fw.close ()
