set -e

. /scripts/00_setup.sh

if [ "${DUMMY}" == "1" ]; then
    echo "*** Enabled dummy mode ***"
    echo "This produces zero size artifacts instantly"

    bin_name="spring.tgz"
    dbg_name="spring.dbg"

    touch "${PUBLISH_DIR}/${bin_name}"
    touch "${PUBLISH_DIR}/${dbg_name}"
    echo "bin_name=${bin_name}" >> "$GITHUB_OUTPUT"
    echo "dbg_name=${dbg_name}" >> "$GITHUB_OUTPUT"

    echo "*** Finished"

    exit 0
fi

echo "ccache pre Statistics:"
ccache -s -v
echo "---------------------------------"
echo "Zeroing ccache statistics..."
ccache -z
echo "---------------------------------"

. /scripts/01_clone.sh
. /scripts/02_configure.sh
. /scripts/03_compile.sh
. /scripts/04_install.sh

if [ ${PUBLISH_ARTIFACTS} ]; then
    . /scripts/05_fill_debugsymbol_dir.sh
    . /scripts/06_fill_build_options_file.sh
    . /scripts/07_pack_build_artifacts.sh
    . /scripts/08_copy_to_publish_dir.sh

    echo "ccache post Statistics:"
    ccache -s -v -x
fi
