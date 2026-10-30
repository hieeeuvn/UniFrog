#!/bin/sh
set -eu

: "${HOSTCC:=cc}"
: "${BUILD:=build}"
: "${CORE_INCLUDE:=.deps/cores/libretro-common/include}"
: "${JS2300:=js2300}"

probe="$BUILD/host-sanitize-probe"
mkdir -p "$BUILD"
if ! printf 'int main(void) { return 0; }\n' |
	"$HOSTCC" -x c - -o "$probe" -fsanitize=address,undefined \
		-fno-omit-frame-pointer >/dev/null 2>&1; then
	echo "SKIP host sanitizers: $HOSTCC does not support them"
	rm -f "$probe"
	exit 0
fi
rm -f "$probe"

binary="$BUILD/host-foundation-sanitize"
RUN_ENV=""
sanitize_build() {
	binary=$1
	shift
	"$HOSTCC" -std=c99 -O1 -g -Wall -Wextra -Iinclude -Itests/host \
		-Ijs2300/include \
		-I"$CORE_INCLUDE" \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		"$@" -o "$binary"
}
sanitize_run() {
	binary=$1
	if [ -n "$RUN_ENV" ]; then
		env $RUN_ENV "$binary"
	else
		"$binary"
	fi
}
sanitize_build_run() {
	sanitize_build "$@"
	sanitize_run "$binary"
}

sanitize_build_run "$binary" tests/host/foundation_test.c \
	foundation/src/runtime/unifrog_path.c \
	foundation/src/runtime/unifrog_artwork.c \
	foundation/src/runtime/unifrog_battery_policy.c \
	foundation/src/runtime/unifrog_boot_policy.c \
	foundation/src/runtime/unifrog_log_policy.c \
	foundation/src/archive/unifrog_zip.c \
	foundation/src/archive/unifrog_zip_writer.c \
	foundation/src/storage/unifrog_storage_profile.c \
	foundation/src/display/unifrog_text.c -lz
sanitize_build_run "$BUILD/host-media-policy-sanitize" \
	tests/host/media_policy_test.c \
	components/media/src/unifrog_media_policy.c \
	components/media/src/unifrog_media_content.c \
	foundation/src/content/unifrog_reader_content.c
sanitize_build_run "$BUILD/host-diagnostic-policy-sanitize" \
	tests/host/diagnostic_policy_test.c \
	components/diagnostics/src/unifrog_diagnostic_policy.c
sanitize_build_run "$BUILD/host-libretro-policy-sanitize" \
	tests/host/libretro_policy_test.c \
	components/libretro/src/unifrog_libretro_policy.c
sanitize_build_run "$BUILD/host-config-sanitize" \
	tests/host/config_test.c \
	foundation/src/config/unifrog_config.c \
	components/frontend/src/app/frontend_config.c \
	components/libretro/src/unifrog_libretro_policy.c
sanitize_build_run "$BUILD/host-frontend-controller-sanitize" \
	tests/host/frontend_controller_test.c \
	components/frontend/src/app/frontend_controller.c \
	components/frontend/src/app/frontend_model.c \
	foundation/src/storage/unifrog_storage_profile.c
sanitize_build_run "$BUILD/host-frontend-services-sanitize" \
	-Icomponents/frontend/src/app \
	-I"$JS2300/include" -I"$BUILD" \
	tests/host/frontend_services_test.c \
	components/frontend/src/app/frontend_services.c
sanitize_build_run "$BUILD/host-core-registry-sanitize" \
	tools/core_registry_check.c \
	components/frontend/src/app/frontend_core_registry.c

# The achievements engine needs the pinned rcheevos checkout, which is an
# untracked external dependency (see docs/retroachievements.md).
if [ -f .deps/rcheevos/src/rc_libretro.c ]; then
	RCHEEVOS_SRC="
	.deps/rcheevos/src/rc_compat.c
	.deps/rcheevos/src/rc_libretro.c
	.deps/rcheevos/src/rc_util.c
	.deps/rcheevos/src/rc_version.c
	.deps/rcheevos/src/rcheevos/alloc.c
	.deps/rcheevos/src/rcheevos/condition.c
	.deps/rcheevos/src/rcheevos/condset.c
	.deps/rcheevos/src/rcheevos/consoleinfo.c
	.deps/rcheevos/src/rcheevos/format.c
	.deps/rcheevos/src/rcheevos/lboard.c
	.deps/rcheevos/src/rcheevos/memref.c
	.deps/rcheevos/src/rcheevos/operand.c
	.deps/rcheevos/src/rcheevos/richpresence.c
	.deps/rcheevos/src/rcheevos/runtime.c
	.deps/rcheevos/src/rcheevos/runtime_progress.c
	.deps/rcheevos/src/rcheevos/trigger.c
	.deps/rcheevos/src/rcheevos/value.c
	.deps/rcheevos/src/rapi/rc_api_common.c
	.deps/rcheevos/src/rapi/rc_api_editor.c
	.deps/rcheevos/src/rapi/rc_api_info.c
	.deps/rcheevos/src/rapi/rc_api_runtime.c
	.deps/rcheevos/src/rapi/rc_api_user.c
	.deps/rcheevos/src/rhash/aes.c
	.deps/rcheevos/src/rhash/cdreader.c
	.deps/rcheevos/src/rhash/hash.c
	.deps/rcheevos/src/rhash/hash_disc.c
	.deps/rcheevos/src/rhash/hash_encrypted.c
	.deps/rcheevos/src/rhash/hash_rom.c
	.deps/rcheevos/src/rhash/hash_zip.c
	.deps/rcheevos/src/rhash/md5.c
	"
	sanitize_build "$BUILD/host-achievements-sanitize" \
		-DRC_NO_THREADS -DRC_DISABLE_LUA \
		-DUNIFROG_SD_ROOT='"/tmp/unifrog-achievements-test"' \
		-I.deps/rcheevos/include -I.deps/rcheevos/src \
		tests/host/achievements_test.c \
		components/libretro/src/achievements/unifrog_achievements.c \
		$RCHEEVOS_SRC -lm
	# rcheevos computes allocation offsets with an offsetof-style null member
	# access macro (RC_ALLOC_WITH_TRAILING). UBSan flags it even though the
	# address is never dereferenced, so silence just that upstream file.
	printf 'null:*condset.c\n' >"$BUILD/ubsan-achievements.supp"
	RUN_ENV="UBSAN_OPTIONS=suppressions=$BUILD/ubsan-achievements.supp" \
		sanitize_run "$binary"
	RUN_ENV=""
else
	echo "SKIP achievements: .deps/rcheevos is not checked out"
fi
echo "OK host sanitizers"
