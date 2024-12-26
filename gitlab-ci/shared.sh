#!/bin/bash

function exec_cmd()
{
	echo "executing $@"
	$@ >/dev/null 2>&1

	if [ $? -ne 0 ]; then
		echo "Failed to execute $@"
		exit 1
	fi
}

function install_bbfdm()
{
	[ -d "/opt/dev/bbfdm" ] && return 0

	if [ -n "${BBFDM_BRANCH}" ]; then
		exec_cmd git clone -b ${BBFDM_BRANCH} https://dev.iopsys.eu/bbf/bbfdm.git /opt/dev/bbfdm
	else
		exec_cmd git clone https://dev.iopsys.eu/bbf/bbfdm.git /opt/dev/bbfdm
	fi

	cd /opt/dev/bbfdm
	./gitlab-ci/install-dependencies.sh
	./gitlab-ci/setup.sh
}
