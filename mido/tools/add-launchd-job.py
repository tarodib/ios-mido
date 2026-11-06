#!/usr/bin/env python3
"""Add (or replace) a launchd job in iOS's cached job list.
  add-launchd-job.py LAUNCHD_PLIST JOB_PLIST
LAUNCHD_PLIST is /System/Library/xpc/launchd.plist on the mounted root (binary plist;
launchd reads it instead of the LaunchDaemons directories when booted with
launchd_unsecure_cache=1). JOB_PLIST is an ordinary launchd job plist; it is stored
under /System/Library/LaunchDaemons/<Label>.plist like Apple's own jobs.
"""
import plistlib
import sys


def main():
    path, job_path = sys.argv[1], sys.argv[2]
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    with open(job_path, 'rb') as f:
        job = plistlib.load(f)
    key = '/System/Library/LaunchDaemons/%s.plist' % job['Label']
    plist['LaunchDaemons'][key] = job
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    print('added', key)


if __name__ == '__main__':
    main()
