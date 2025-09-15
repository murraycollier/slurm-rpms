#!/usr/bin/env bash

wget https://download.schedmd.com/slurm/slurm-25.05.3.tar.bz2

rpmbuild -ta slurm-25.05.3.tar.bz2
