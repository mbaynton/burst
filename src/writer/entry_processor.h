/*
 * Entry Processor - Process individual file system entries for archiving
 *
 * This module handles the processing of files, directories, and symlinks
 * for addition to BURST archives. Extracted from main.c for testability.
 */
#ifndef ENTRY_PROCESSOR_H
#define ENTRY_PROCESSOR_H

#include <stdbool.h>
#include <sys/stat.h>

struct burst_writer;

/*
 * Process a single file system entry and add it to the archive.
 *
 * Parameters:
 *   writer         - The burst_writer instance
 *   input_path     - Full path to the file/directory/symlink on disk
 *   archive_name   - Name to use in the archive
 *   symlink_target - Target path for symlinks (NULL for files/directories)
 *   file_stat      - stat structure for the entry
 *   is_dir         - true if this is a directory entry
 *
 * Returns:
 *   1 on success (entry was added to archive)
 *   0 on failure (entry was skipped)
 */
int process_entry(struct burst_writer *writer,
                  const char *input_path,
                  const char *archive_name,
                  const char *symlink_target,
                  const struct stat *file_stat,
                  bool is_dir);

#endif /* ENTRY_PROCESSOR_H */
