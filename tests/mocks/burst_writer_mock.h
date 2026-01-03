/*
 * Mock header for burst_writer functions
 * Used to test error handling in entry_processor.c
 *
 * Note: We use void* for struct pointers to avoid CMock trying to
 * take sizeof() on incomplete types. The actual functions use the
 * real struct pointer types, but CMock only sees these prototypes.
 */
#ifndef BURST_WRITER_MOCK_H
#define BURST_WRITER_MOCK_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

int burst_writer_add_file(void *writer,
                          FILE *input_file,
                          void *lfh,
                          int lfh_len,
                          bool is_header_only,
                          uint32_t unix_mode,
                          uint32_t uid,
                          uint32_t gid);

int burst_writer_add_symlink(void *writer,
                              void *lfh,
                              int lfh_len,
                              const char *target,
                              size_t target_len,
                              uint32_t unix_mode,
                              uint32_t uid,
                              uint32_t gid);

int burst_writer_add_directory(void *writer,
                                void *lfh,
                                int lfh_len,
                                uint32_t unix_mode,
                                uint32_t uid,
                                uint32_t gid);

/* Also mock burst_writer_write which is called by zip_structures.c */
int burst_writer_write(void *writer,
                       const void *data,
                       size_t len);

#endif /* BURST_WRITER_MOCK_H */
