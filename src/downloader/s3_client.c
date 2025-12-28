#include "s3_client.h"
#include "burst_downloader.h"

#include <aws/auth/credentials.h>
#include <aws/common/allocator.h>
#include <aws/common/byte_buf.h>
#include <aws/common/command_line_parser.h>
#include <aws/common/condition_variable.h>
#include <aws/common/error.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/s3/s3_client.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int s3_client_init(struct burst_downloader *downloader) {
    if (!downloader) {
        fprintf(stderr, "Error: NULL downloader\n");
        return -1;
    }

    // Get default allocator
    downloader->allocator = aws_default_allocator();

    // Initialize S3 library
    aws_s3_library_init(downloader->allocator);

    // Create event loop group for async I/O
    downloader->event_loop_group = aws_event_loop_group_new_default(downloader->allocator, 0, NULL);
    if (!downloader->event_loop_group) {
        fprintf(stderr, "Error: Failed to create event loop group: %s\n",
                aws_error_debug_str(aws_last_error()));
        goto error_cleanup;
    }

    // Create DNS resolver
    struct aws_host_resolver_default_options resolver_options = {
        .el_group = downloader->event_loop_group,
        .max_entries = 8,
    };
    downloader->host_resolver = aws_host_resolver_new_default(downloader->allocator, &resolver_options);
    if (!downloader->host_resolver) {
        fprintf(stderr, "Error: Failed to create host resolver: %s\n",
                aws_error_debug_str(aws_last_error()));
        goto error_cleanup;
    }

    // Create client bootstrap
    struct aws_client_bootstrap_options bootstrap_options = {
        .event_loop_group = downloader->event_loop_group,
        .host_resolver = downloader->host_resolver,
    };
    downloader->client_bootstrap = aws_client_bootstrap_new(downloader->allocator, &bootstrap_options);
    if (!downloader->client_bootstrap) {
        fprintf(stderr, "Error: Failed to create client bootstrap: %s\n",
                aws_error_debug_str(aws_last_error()));
        goto error_cleanup;
    }

    // Create TLS context (required for SSO and other secure providers)
    struct aws_tls_ctx_options tls_ctx_options;
    aws_tls_ctx_options_init_default_client(&tls_ctx_options, downloader->allocator);
    downloader->tls_ctx = aws_tls_client_ctx_new(downloader->allocator, &tls_ctx_options);
    aws_tls_ctx_options_clean_up(&tls_ctx_options);

    if (!downloader->tls_ctx) {
        fprintf(stderr, "Error: Failed to create TLS context: %s\n",
                aws_error_debug_str(aws_last_error()));
        goto error_cleanup;
    }

    // Determine profile name: provided > env var > "default"
    const char *profile_to_use = downloader->profile_name;
    if (!profile_to_use) {
        profile_to_use = getenv("AWS_PROFILE");
    }
    if (!profile_to_use) {
        profile_to_use = "default";
    }
    struct aws_byte_cursor profile_cursor = aws_byte_cursor_from_c_str(profile_to_use);

    // Build custom credentials provider chain with SSO
    // Priority: env vars > SSO > profile > STS web identity > ECS > IMDS

    // 1. Environment variables provider
    struct aws_credentials_provider_environment_options env_options = {.shutdown_options = {0}};
    struct aws_credentials_provider *env_provider =
        aws_credentials_provider_new_environment(downloader->allocator, &env_options);

    // 2. SSO provider (NEW!)
    struct aws_credentials_provider_sso_options sso_options = {
        .shutdown_options = {0},
        .bootstrap = downloader->client_bootstrap,
        .tls_ctx = downloader->tls_ctx,
        .profile_name_override = profile_cursor,
    };
    struct aws_credentials_provider *sso_provider =
        aws_credentials_provider_new_sso(downloader->allocator, &sso_options);

    // 3. Profile provider (static credentials and assume-role)
    struct aws_credentials_provider_profile_options profile_options = {
        .shutdown_options = {0},
        .bootstrap = downloader->client_bootstrap,
        .tls_ctx = downloader->tls_ctx,
        .profile_name_override = profile_cursor,
    };
    struct aws_credentials_provider *profile_provider =
        aws_credentials_provider_new_profile(downloader->allocator, &profile_options);

    // 4. STS web identity provider
    struct aws_credentials_provider_sts_web_identity_options web_identity_options = {
        .shutdown_options = {0},
        .bootstrap = downloader->client_bootstrap,
        .tls_ctx = downloader->tls_ctx,
    };
    struct aws_credentials_provider *web_identity_provider =
        aws_credentials_provider_new_sts_web_identity(downloader->allocator, &web_identity_options);

    // 5. ECS provider
    struct aws_credentials_provider_ecs_environment_options ecs_options = {
        .shutdown_options = {0},
        .bootstrap = downloader->client_bootstrap,
        .tls_ctx = downloader->tls_ctx,
    };
    struct aws_credentials_provider *ecs_provider =
        aws_credentials_provider_new_ecs_from_environment(downloader->allocator, &ecs_options);

    // 6. IMDS provider (EC2 instance metadata)
    struct aws_credentials_provider_imds_options imds_options = {
        .shutdown_options = {0},
        .bootstrap = downloader->client_bootstrap,
        .imds_version = IMDS_PROTOCOL_V2,
    };
    struct aws_credentials_provider *imds_provider =
        aws_credentials_provider_new_imds(downloader->allocator, &imds_options);

    // Build the chain array, filtering out NULL providers
    struct aws_credentials_provider *all_providers[] = {
        env_provider,
        sso_provider,
        profile_provider,
        web_identity_provider,
        ecs_provider,
        imds_provider,
    };

    // Count non-NULL providers and build filtered array
    struct aws_credentials_provider *providers[6];
    size_t provider_count = 0;
    for (size_t i = 0; i < sizeof(all_providers) / sizeof(all_providers[0]); i++) {
        if (all_providers[i] != NULL) {
            providers[provider_count++] = all_providers[i];
        }
    }

    if (provider_count == 0) {
        fprintf(stderr, "Error: No credentials providers could be initialized\n");
        goto error_cleanup;
    }

    struct aws_credentials_provider_chain_options chain_options = {
        .shutdown_options = {0},
        .providers = providers,
        .provider_count = provider_count,
    };

    downloader->credentials_provider =
        aws_credentials_provider_new_chain(downloader->allocator, &chain_options);

    // Release individual provider references (chain now owns them)
    // Only release non-NULL providers
    if (env_provider) aws_credentials_provider_release(env_provider);
    if (sso_provider) aws_credentials_provider_release(sso_provider);
    if (profile_provider) aws_credentials_provider_release(profile_provider);
    if (web_identity_provider) aws_credentials_provider_release(web_identity_provider);
    if (ecs_provider) aws_credentials_provider_release(ecs_provider);
    if (imds_provider) aws_credentials_provider_release(imds_provider);

    if (!downloader->credentials_provider) {
        fprintf(stderr, "Error: Failed to create credentials provider chain: %s\n",
                aws_error_debug_str(aws_last_error()));
        goto error_cleanup;
    }

    printf("Using AWS profile: %s\n", profile_to_use);

    // Create signing config
    struct aws_signing_config_aws *signing_config =
        aws_mem_calloc(downloader->allocator, 1, sizeof(struct aws_signing_config_aws));
    if (!signing_config) {
        fprintf(stderr, "Error: Failed to allocate signing config\n");
        goto error_cleanup;
    }

    aws_s3_init_default_signing_config(
        signing_config,
        aws_byte_cursor_from_c_str(downloader->region),
        downloader->credentials_provider
    );
    signing_config->flags.use_double_uri_encode = false;

    // Create S3 client with optimal EC2->S3 configuration
    struct aws_s3_client_config client_config = {
        .client_bootstrap = downloader->client_bootstrap,
        .region = aws_byte_cursor_from_c_str(downloader->region),
        .signing_config = signing_config,
        .max_active_connections_override = downloader->max_concurrent_connections,
        .memory_limit_in_bytes = 1024 * 1024 * 1024,  // 1 GiB (AWS CRT minimum)
        .part_size = 8 * 1024 * 1024,  // 8 MiB (matches BURST alignment)
        .throughput_target_gbps = 10.0,  // EC2 enhanced networking
        .enable_read_backpressure = false,
    };

    downloader->s3_client = aws_s3_client_new(downloader->allocator, &client_config);
    if (!downloader->s3_client) {
        fprintf(stderr, "Error: Failed to create S3 client: %s\n",
                aws_error_debug_str(aws_last_error()));
        aws_mem_release(downloader->allocator, signing_config);
        goto error_cleanup;
    }

    // Signing config is copied by aws_s3_client_new, so we can free it
    aws_mem_release(downloader->allocator, signing_config);

    return 0;

error_cleanup:
    s3_client_cleanup(downloader);
    return -1;
}

void s3_client_cleanup(struct burst_downloader *downloader) {
    if (!downloader) {
        return;
    }

    // Release resources in reverse order of creation
    if (downloader->s3_client) {
        aws_s3_client_release(downloader->s3_client);
        downloader->s3_client = NULL;
    }

    if (downloader->credentials_provider) {
        aws_credentials_provider_release(downloader->credentials_provider);
        downloader->credentials_provider = NULL;
    }

    if (downloader->tls_ctx) {
        aws_tls_ctx_release(downloader->tls_ctx);
        downloader->tls_ctx = NULL;
    }

    if (downloader->client_bootstrap) {
        aws_client_bootstrap_release(downloader->client_bootstrap);
        downloader->client_bootstrap = NULL;
    }

    if (downloader->host_resolver) {
        aws_host_resolver_release(downloader->host_resolver);
        downloader->host_resolver = NULL;
    }

    if (downloader->event_loop_group) {
        aws_event_loop_group_release(downloader->event_loop_group);
        downloader->event_loop_group = NULL;
    }

    // Clean up S3 library
    aws_s3_library_clean_up();
}
