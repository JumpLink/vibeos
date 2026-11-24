# VibeosUnifiedPush (../vibeos-unified-push.h): an app's push messages through the system's UnifiedPush distributor,
# decrypted (RFC 8291) with GnuTLS, which the image has already (GTK's print support and glib-networking load it).
# It goes into libvibeos whole. The sending side, the tests and the two test tools are not installed.
gnutls_dep = dependency('gnutls', version: '>= 3.8.2')    # gnutls_privkey_derive_secret
push_deps = [dependency('gio-unix-2.0', version: '>= 2.76'), gnutls_dep]
push_inc = include_directories('..')

vibeos_unified_push_lib = static_library('vibeos-unified-push', 'vibeos-unified-push.c', 'vibeos-webpush.c',
  c_args: ['-DG_LOG_DOMAIN="VibeosUnifiedPush"'], dependencies: push_deps, include_directories: push_inc, pic: true)
install_headers('../vibeos-unified-push.h', subdir: 'vibeos-1')

# encryption and VAPID, as an app server does them
webpush_send_lib = static_library('vibeos-webpush-send', 'vibeos-webpush-send.c', link_with: vibeos_unified_push_lib,
  dependencies: push_deps)

# RFC 8291's example both ways, what must be refused, fresh keys, VAPID
test('webpush', executable('test-webpush', 'test-webpush.c', link_with: [webpush_send_lib, vibeos_unified_push_lib],
                           dependencies: push_deps))

# the connector against a distributor of the test's own, on a private bus (GTestDBus)
test('unified-push', executable('test-unified-push', 'test-unified-push.c',
                                link_with: [webpush_send_lib, vibeos_unified_push_lib], dependencies: push_deps,
                                include_directories: push_inc),
     env: ['GSETTINGS_BACKEND=memory', 'GIO_USE_VFS=local', 'NO_AT_BRIDGE=1'], timeout: 60)

# for trying push by hand (tools/unifiedpush/e2e.sh): an app that registers and prints its messages, and a sender
# that encrypts a message for it and sends it to its endpoint with a VAPID header
executable('vibeos-push-test', 'vibeos-push-test.c', link_with: vibeos_unified_push_lib, dependencies: push_deps,
           include_directories: push_inc)
soup_dep = dependency('libsoup-3.0', required: false)
if soup_dep.found()
  executable('vibeos-push-send', 'vibeos-push-send.c', link_with: [webpush_send_lib, vibeos_unified_push_lib],
             dependencies: push_deps + [soup_dep])
endif
