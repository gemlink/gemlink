# Release Process

Meta: There should always be a single release engineer to disambiguate responsibility.

If this is a hotfix release, please see `./hotfix-process.md` before proceeding.

## Pre-release

### Github Milestone

Ensure all goals for the github milestone are met. If not, remove tickets
or PRs with a comment as to why it is not included. (Running out of time
is a common reason.)

### Pre-release checklist:

Check that dependencies are properly hosted by looking at the `check-depends` builder:

https://ci.gemlink.org/#/builders/1

Check that there are no surprising performance regressions:

https://speed.gemlink.org

Ensure that new performance metrics appear on that site.

### Protocol Safety Checks:

If this release changes the behavior of the protocol or fixes a serious
bug, verify that a pre-release PR merge updated `PROTOCOL_VERSION` in
`version.h` correctly.

If this release breaks backwards compatibility or needs to prevent
interaction with software forked projects, change the network magic
numbers. Set the four `pchMessageStart` in `CTestNetParams` in
`chainparams.cpp` to random values.

Both of these should be done in standard PRs ahead of the release
process. If these were not anticipated correctly, this could block the
release, so if you suspect this is necessary, double check with the
whole engineering team.

## Release dependencies

The release script has the following dependencies:

- `help2man`
- `debchange` (part of the devscripts Debian package)

You can optionally install the `progressbar2` Python module with pip to have a
progress bar displayed during the build process.

## Release process

In the commands below, <RELEASE> and <RELEASE_PREV> are prefixed with a v, ie.
v1.0.9 (not 1.0.9).

### Create the release branch

Run the release script, which will verify you are on the latest clean
checkout of master, create a branch, then commit standard automated
changes to that branch locally:

    $ ./zcutil/make-release.py <RELEASE> <RELEASE_PREV> <APPROX_RELEASE_HEIGHT>

Example:

    $ ./zcutil/make-release.py v1.0.9 v1.0.8-1 120000

### Create, Review, and Merge the release branch pull request

Review the automated changes in git:

    $ git log master..HEAD

Push the resulting branch to github:

    $ git push 'git@github.com:$YOUR_GITHUB_NAME/gemlink' $(git rev-parse --abbrev-ref HEAD)

Then create the PR on github. Complete the standard review process,
then merge, then wait for CI to complete.

## Make tag for the newly merged result

Checkout master and pull the latest version to ensure master is up to date with the release PR which was merged in before.

    $ git checkout master
    $ git pull --ff-only

Check the last commit on the local and remote versions of master to make sure they are the same:

    $ git log -1

The output should include something like, which is created by Homu:

    Auto merge of #4242 - nathan-at-least:release-v1.0.9, r=nathan-at-least

Then create the git tag. The `-s` means the release tag will be
signed. **CAUTION:** Remember the `v` at the beginning here:

    $ git tag -s v1.0.9
    $ git push origin v1.0.9

## Make and deploy deterministic builds

- Run the [Gitian deterministic build environment](https://github.com/gemlink/gemlink-gitian)
- Compare the uploaded [build manifests on gitian.sigs](https://github.com/gemlink/gitian.sigs)
- If all is well, the DevOps engineer will build the Debian packages and update the
  [apt.gemlink.org package repository](https://apt.gemlink.org).

## Add release notes to GitHub

- Go to the [GitHub tags page](https://github.com/gemlink/gemlink/tags).
- Click "Add release notes" beside the tag for this release.
- Copy the release blog post into the release description, and edit to suit
  publication on GitHub. See previous release notes for examples.
- Click "Publish release" if publishing the release blog post now, or
  "Save draft" to store the notes internally (and then return later to publish
  once the blog post is up).

Note that some GitHub releases are marked as "Verified", and others as
"Unverified". This is related to the GPG signature on the release tag - in
particular, GitHub needs the corresponding public key to be uploaded to a
corresponding GitHub account. If this release is marked as "Unverified", click
the marking to see what GitHub wants to be done.

## Post Release Task List

### Deploy testnet

Notify the Gemlink DevOps engineer/sysadmin that the release has been tagged. They update some variables in the company's automation code and then run an Ansible playbook, which:

- builds Gemlink based on the specified branch
- deploys it as a public service (e.g. betatestnet.gemlink.org, mainnet.gemlink.org)
- often the same server can be re-used, and the role idempotently handles upgrades, but if not then they also need to update DNS records
- possible manual steps: blowing away the `testnet3` dir, deleting old parameters, restarting DNS seeder

Then, verify that nodes can connect to the testnet server, and update the guide on the wiki to ensure the correct hostname is listed in the recommended gemlink.conf.

### Update the 1.0 User Guide

This also means updating [the translations](https://github.com/gemlink/gemlink-docs).
Coordinate with the translation team for now. Suggestions for improving this
part of the process should be added to #2596.

### Publish the release announcement (blog, github, gemlink-dev, slack)

## Celebrate
