#ifndef S3_CLIENT_H
#define S3_CLIENT_H

#include "burst_downloader.h"

// Initialize AWS CRT and create S3 client
// Returns 0 on success, -1 on failure
int s3_client_init(struct burst_downloader *downloader);

// Cleanup and destroy S3 client
void s3_client_cleanup(struct burst_downloader *downloader);

#endif // S3_CLIENT_H
