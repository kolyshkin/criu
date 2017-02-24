#!/bin/sh
# A test to make sure "make uninstall" works as intended.

set -e
SELFDIR=$(dirname $(realpath $0))
DESTDIR=$SELFDIR/test.install-$$
cd $SELFDIR/../../..

set -x
make install DESTDIR=$DESTDIR
make uninstall DESTDIR=$DESTDIR
set +x

# There should be no files left (directories are OK for now)
if [ $(find $DESTDIR -type f | wc -l) -gt 0 ]; then
	echo "Files left after uninstall:"
	find $DESTDIR -type f
	echo "FAIL"
	exit 1
fi

echo PASS
