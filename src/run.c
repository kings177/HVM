#include "hvm.c"

// Readback: λ-Encoded Ctr
typedef struct Ctr {
  u32  tag;
  u32  args_len;
  Port args_buf[16];
} Ctr;

// Readback: λ-Encoded Str (UTF-32)
// FIXME: this is actually ASCII :|
// FIXME: remove len limit
typedef struct Str {
  u32  text_len;
  char text_buf[256];
} Str;

// IO Magic Number
#define IO_MAGIC_0 0xD0CA11
#define IO_MAGIC_1 0xFF1FF1

// IO Tags
#define IO_DONE 0
#define IO_CALL 1

// List Type
#define LIST_NIL  0
#define LIST_CONS 1

// Readback
// --------

// Reads back a λ-Encoded constructor from device to host.
// Encoding: λt ((((t TAG) arg0) arg1) ...)
Ctr readback_ctr(Net* net, Book* book, Port port) {
  Ctr ctr;
  ctr.tag = -1;
  ctr.args_len = 0;

  // Loads root lambda
  Port lam_port = expand(net, book, port);
  if (get_tag(lam_port) != CON) return ctr;
  Pair lam_node = node_load(net, get_val(lam_port));

  // Loads first application
  Port app_port = expand(net, book, get_fst(lam_node));
  if (get_tag(app_port) != CON) return ctr;
  Pair app_node = node_load(net, get_val(app_port));

  // Loads first argument (as the tag)
  Port arg_port = expand(net, book, get_fst(app_node));
  if (get_tag(arg_port) != NUM) return ctr;
  ctr.tag = get_u24(get_val(arg_port));

  // Loads remaining arguments
  while (TRUE) {
    app_port = expand(net, book, get_snd(app_node));
    if (get_tag(app_port) != CON) break;
    app_node = node_load(net, get_val(app_port));
    arg_port = expand(net, book, get_fst(app_node));
    ctr.args_buf[ctr.args_len++] = arg_port;
  }

  return ctr;
}

// Converts a UTF-32 (truncated to 24 bits) string to a Port.
// Since unicode scalars can fit in 21 bits, HVM's u24
// integers can contain any unicode scalar value.
// Encoding:
// - λt (t NIL)
// - λt (((t CONS) head) tail)
Str readback_str(Net* net, Book* book, Port port) {
  // Result
  Str str;
  str.text_len = 0;

  // Readback loop
  while (TRUE) {
    // Normalizes the net
    normalize(net, book);

    //printf("reading str %s\n", show_port(peek(net, port)).x);

    // Reads the λ-Encoded Ctr
    Ctr ctr = readback_ctr(net, book, peek(net, port));

    //printf("reading tag %d | len %d\n", ctr.tag, ctr.args_len);

    // Reads string layer
    switch (ctr.tag) {
      case LIST_NIL: {
        break;
      }
      case LIST_CONS: {
        if (ctr.args_len != 2) break;
        if (get_tag(ctr.args_buf[0]) != NUM) break;
        if (str.text_len >= 256) { printf("ERROR: for now, HVM can only readback strings of length <256."); break; }
        //printf("reading chr %d\n", get_u24(get_val(ctr.args_buf[0])));
        str.text_buf[str.text_len++] = get_u24(get_val(ctr.args_buf[0]));
        boot_redex(net, new_pair(ctr.args_buf[1], ROOT));
        port = ROOT;
        continue;
      }
    }
    break;
  }

  str.text_buf[str.text_len] = '\0';

  return str;
}

/// Returns a λ-Encoded Ctr for a NIL: λt (t NIL)
/// Should only be called within `inject_str`, as a previous call
/// to `get_resources` is expected.
Port inject_nil(Net* net) {
  u32 v1 = tm[0]->vloc[0];

  u32 n1 = tm[0]->nloc[0];
  u32 n2 = tm[0]->nloc[1];

  vars_create(net, v1, NONE);
  Port var = new_port(VAR, v1);

  node_create(net, n1, new_pair(new_port(NUM, new_u24(LIST_NIL)), var));
  node_create(net, n2, new_pair(new_port(CON, n1), var));

  return new_port(CON, n2);
}

/// Returns a λ-Encoded Ctr for a CONS: λt (((t CONS) head) tail)
/// Should only be called within `inject_str`, as a previous call
/// to `get_resources` is expected.
/// The `char_idx` parameter is used to offset the vloc and nloc
/// allocations, otherwise they would conflict with each other on
/// subsequent calls.
Port inject_cons(Net* net, Port head, Port tail, u32 char_idx) {
  u32 v1 = tm[0]->vloc[1 + char_idx];

  u32 n1 = tm[0]->nloc[2 + char_idx * 4 + 0];
  u32 n2 = tm[0]->nloc[2 + char_idx * 4 + 1];
  u32 n3 = tm[0]->nloc[2 + char_idx * 4 + 2];
  u32 n4 = tm[0]->nloc[2 + char_idx * 4 + 3];

  vars_create(net, v1, NONE);
  Port var = new_port(VAR, v1);

  node_create(net, n1, new_pair(tail, var));
  node_create(net, n2, new_pair(head, new_port(CON, n1)));
  node_create(net, n3, new_pair(new_port(NUM, new_u24(LIST_CONS)), new_port(CON, n2)));
  node_create(net, n4, new_pair(new_port(CON, n3), var));

  return new_port(CON, n4);
}

// Converts a UTF-32 (truncated to 24 bits) string to a Port.
// Since unicode scalars can fit in 21 bits, HVM's u24
// integers can contain any unicode scalar value.
// Encoding:
// - λt (t NIL)
// - λt (((t CONS) head) tail)
Port inject_str(Net* net, Str *str) {
  // Allocate all resources up front:
  // - NIL needs  2 nodes & 1 var
  // - CONS needs 4 nodes & 1 var
  u32 len = str->text_len;
  if (!get_resources(net, tm[0], 0, 2 + 4 * len, 1 + len)) {
    printf("inject_str: failed to get resources\n");
    return new_port(ERA, 0);
  }

  Port port = inject_nil(net);

  for (u32 i = 0; i < len; i++) {
    Port chr = new_port(NUM, new_u24(str->text_buf[len - i - 1]));
    port = inject_cons(net, chr, port, i);
  }

  return port;
}

// Primitive IO Fns
// -----------------

// Open file pointers. Indices into this array
// are used as "file descriptors".
// Indices 0 1 and 2 are reserved.
// - 0 -> stdin
// - 1 -> stdout
// - 2 -> stderr
static FILE* FILE_POINTERS[256];

// Converts a NUM port (file descriptor) to file pointer.
FILE* readback_file(Port port) {
  if (get_tag(port) != NUM) {
    fprintf(stderr, "non-num where file descriptor was expected: %i\n", get_tag(port));
    return NULL;
  }

  u32 idx = get_u24(get_val(port));

  if (idx == 0) return stdin;
  if (idx == 1) return stdout;
  if (idx == 2) return stderr;

  FILE* fp = FILE_POINTERS[idx];
  if (fp == NULL) {
    fprintf(stderr, "invalid file descriptor\n");
    return NULL;
  }

  return fp;
}

// Reads a single char from `argm`.
Port io_read_char(Net* net, Book* book, Port argm) {
  FILE* fp = readback_file(peek(net, argm));
  if (fp == NULL) {
    return new_port(ERA, 0);
  }

  /// Read a string.
  Str str;

  str.text_buf[0] = fgetc(fp);
  str.text_buf[1] = 0;
  str.text_len = 1;

  return inject_str(net, &str);
}

// Reads from `argm` at most 255 characters or until a newline is seen.
Port io_read_line(Net* net, Book* book, Port argm) {
  FILE* fp = readback_file(peek(net, argm));
  if (fp == NULL) {
    fprintf(stderr, "io_read_line: invalid file descriptor\n");
    return new_port(ERA, 0);
  }

  /// Read a string.
  Str str;

  if (fgets(str.text_buf, sizeof(str.text_buf), fp) == NULL) {
    fprintf(stderr, "io_read_line: failed to read\n");
  }
  str.text_len = strlen(str.text_buf);

  // Strip any trailing newline.
  if (str.text_len > 0 && str.text_buf[str.text_len - 1] == '\n') {
    str.text_buf[str.text_len] = 0;
    str.text_len--;
  }

  // Convert it to a port.
  return inject_str(net, &str);
}

// Opens a file with the provided mode.
// `argm` is a tuple (CON node) of the
// file name and mode as strings.
Port io_open_file(Net* net, Book* book, Port argm) {
  if (get_tag(peek(net, argm)) != CON) {
    fprintf(stderr, "io_open_file: expected tuple\n");
    return new_port(ERA, 0);
  }

  Pair args = node_load(net, get_val(argm));
  Str name = readback_str(net, book, get_fst(args));
  Str mode = readback_str(net, book, get_snd(args));

  for (u32 fd = 3; fd < sizeof(FILE_POINTERS); fd++) {
    if (FILE_POINTERS[fd] == NULL) {
      FILE_POINTERS[fd] = fopen(name.text_buf, mode.text_buf);
      return new_port(NUM, new_u24(fd));
    }
  }

  fprintf(stderr, "io_open_file: too many open files\n");

  return new_port(ERA, 0);
}

// Closes a file, reclaiming the file descriptor.
Port io_close_file(Net* net, Book* book, Port argm) {
  FILE* fp = readback_file(peek(net, argm));
  if (fp == NULL) {
    fprintf(stderr, "io_close_file: failed to close\n");
    return new_port(ERA, 0);
  }

  int err = fclose(fp) != 0;
  if (err != 0) {
    fprintf(stderr, "io_close_file: failed to close: %i\n", err);
    return new_port(ERA, 0);
  }

  FILE_POINTERS[get_u24(get_val(argm))] = NULL;

  return new_port(ERA, 0);
}

// Writes a string to a file.
// `argm` is a tuple (CON node) of the
// file descriptor and string to write.
Port io_write(Net* net, Book* book, Port argm) {
  if (get_tag(peek(net, argm)) != CON) {
    fprintf(stderr, "io_write: expected tuple, but got %u\n", get_tag(peek(net, argm)));
    return new_port(ERA, 0);
  }

  Pair args = node_load(net, get_val(argm));
  FILE* fp = readback_file(peek(net, get_fst(args)));
  Str str = readback_str(net, book, get_snd(args));

  if (fp == NULL) {
    fprintf(stderr, "io_write: invalid file descriptor\n");
    return new_port(ERA, 0);
  }

  if (fputs(str.text_buf, fp) == EOF) {
    fprintf(stderr, "io_write: failed to write\n");
  }

  return new_port(ERA, 0);
}

// Returns the current time as a tuple of the high
// and low 24 bits of a 48-bit nanosecond timestamp.
Port io_get_time(Net* net, Book* book, Port argm) {
  // Get the current time in nanoseconds
  u64 time_ns = time64();
  // Encode the time as a 64-bit unsigned integer
  u32 time_hi = (u32)(time_ns >> 24) & 0xFFFFFFF;
  u32 time_lo = (u32)(time_ns & 0xFFFFFFF);
  // Allocate a node to store the time
  u32 lps = 0;
  u32 loc = node_alloc_1(net, tm[0], &lps);
  node_create(net, loc, new_pair(new_port(NUM, new_u24(time_hi)), new_port(NUM, new_u24(time_lo))));
  // Return the encoded time
  return new_port(CON, loc);
}

// Sleeps.
// `argm` is a tuple (CON node) of the high and low
// 24 bits for a 48-bit duration in nanoseconds.
Port io_sleep(Net* net, Book* book, Port argm) {
  // Get the sleep duration node
  Pair dur_node = node_load(net, get_val(argm));
  // Get the high and low 24-bit parts of the duration
  u32 dur_hi = get_u24(get_val(get_fst(dur_node)));
  u32 dur_lo = get_u24(get_val(get_snd(dur_node)));
  // Combine into a 48-bit duration in nanoseconds
  u64 dur_ns = (((u64)dur_hi) << 24) | dur_lo;
  // Sleep for the specified duration
  struct timespec ts;
  ts.tv_sec = dur_ns / 1000000000;
  ts.tv_nsec = dur_ns % 1000000000;
  nanosleep(&ts, NULL);
  // Return an eraser
  return new_port(ERA, 0);
}

// Book Loader
// -----------

void book_init(Book* book) {
  book->ffns_buf[book->ffns_len++] = (FFn){"READ_CHAR", io_read_char};
  book->ffns_buf[book->ffns_len++] = (FFn){"READ_LINE", io_read_line};
  book->ffns_buf[book->ffns_len++] = (FFn){"OPEN_FILE", io_open_file};
  book->ffns_buf[book->ffns_len++] = (FFn){"CLOSE_FILE", io_close_file};
  book->ffns_buf[book->ffns_len++] = (FFn){"WRITE", io_write};
  book->ffns_buf[book->ffns_len++] = (FFn){"GET_TIME", io_get_time};
  book->ffns_buf[book->ffns_len++] = (FFn){"SLEEP", io_sleep};
}

// Monadic IO Evaluator
// ---------------------

// Runs an IO computation.
void do_run_io(Net* net, Book* book, Port port) {
   book_init(book);

  // IO loop
  while (TRUE) {
    // Normalizes the net
    normalize(net, book);

    // Reads the λ-Encoded Ctr
    Ctr ctr = readback_ctr(net, book, peek(net, port));

    // Checks if IO Magic Number is a CON
    if (get_tag(ctr.args_buf[0]) != CON) {
      break;
    }

    // Checks the IO Magic Number
    Pair io_magic = node_load(net, get_val(ctr.args_buf[0]));
    //printf("%08x %08x\n", get_u24(get_val(get_fst(io_magic))), get_u24(get_val(get_snd(io_magic))));
    if (get_val(get_fst(io_magic)) != new_u24(IO_MAGIC_0) || get_val(get_snd(io_magic)) != new_u24(IO_MAGIC_1)) {
      break;
    }

    switch (ctr.tag) {
      case IO_CALL: {
        Str  func = readback_str(net, book, ctr.args_buf[1]);
        Port argm = ctr.args_buf[2];
        Port cont = ctr.args_buf[3];
        u32  lps  = 0;
        u32  loc  = node_alloc_1(net, tm[0], &lps);
        Port ret  = new_port(ERA, 0);
        FFn* ffn  = NULL;
        // FIXME: optimize this linear search
        for (u32 fid = 0; fid < book->ffns_len; ++fid) {
          if (strcmp(func.text_buf, book->ffns_buf[fid].name) == 0) {
            ffn = &book->ffns_buf[fid];
            break;
          }
        }
        if (ffn == NULL) {
          break;
        }
        ret = ffn->func(net, book, argm);
        node_create(net, loc, new_pair(ret, ROOT));
        boot_redex(net, new_pair(new_port(CON, loc), cont));
        port = ROOT;
        continue;
      }
      case IO_DONE: {
        printf("DONE\n");
        break;
      }
    }
    break;
  }
}
