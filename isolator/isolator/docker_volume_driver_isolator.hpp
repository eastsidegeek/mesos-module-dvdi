
/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_DOCKER_VOLUME_DRIVER_ISOLATOR_HPP_
#define SRC_DOCKER_VOLUME_DRIVER_ISOLATOR_HPP_
#include <iostream>
#include <boost/functional/hash.hpp>
#include <boost/algorithm/string.hpp>
#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/multihashmap.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include <slave/flags.hpp>
#include <slave/containerizer/isolator.hpp>

#include "interface.hpp"
using namespace emccode::isolator::mount;


namespace mesos {
namespace slave {

static constexpr char REXRAY_MOUNT_PREFIX[]       = "/var/lib/rexray/volumes/";
static constexpr char DVDCLI_MOUNT_CMD[]          = "/usr/bin/dvdcli mount";
static constexpr char DVDCLI_UNMOUNT_CMD[]        = "/usr/bin/dvdcli unmount";

static constexpr char VOL_NAME_CMD_OPTION[]       = "--volumename=";
static constexpr char VOL_DRIVER_CMD_OPTION[]     = "--volumedriver=";
static constexpr char VOL_OPTS_CMD_OPTION[]       = "--volumeopts=";
static constexpr char VOL_DRIVER_DEFAULT[]        = "rexray";

static constexpr char VOL_NAME_ENV_VAR_NAME[]     = "DVDI_VOLUME_NAME";
static constexpr char VOL_DRIVER_ENV_VAR_NAME[]   = "DVDI_VOLUME_DRIVER";
static constexpr char VOL_OPTS_ENV_VAR_NAME[]     = "DVDI_VOLUME_OPTS";
static constexpr char JSON_VOLS_ENV_VAR_NAME[]    = "DVDI_VOLS_JSON_ARRAY";

//TODO this is temporary until the working_dir is exposed by mesosphere dev
static constexpr char DVDI_MOUNTLIST_DEFAULT_DIR[]= "/tmp/mesos/";
static constexpr char DVDI_MOUNTLIST_FILENAME[]   = "dvdimounts.pb";
static constexpr char DVDI_WORKDIR_PARAM_NAME[]   = "work_dir";

//TODO this is temporary until the working_dir is exposed by mesosphere dev
static constexpr char DEFAULT_WORKING_DIR[]       = "/tmp/mesos";


class DockerVolumeDriverIsolator: public mesos::slave::Isolator
{
public:
  static Try<mesos::slave::Isolator*> create(const Parameters& parameters);

  virtual ~DockerVolumeDriverIsolator();

  // Slave recovery is a feature of Mesos that allows task/executors
  // to keep running if a slave process goes down, AND
  // allows the slave process to reconnect with already running
  // slaves when it restartss
  virtual process::Future<Nothing> recover(
    const std::list<ContainerState>& states,
    const hashset<ContainerID>& orphans);

  // Prepare runs BEFORE a task is started
  // will check if the volume is already mounted and if not,
  // will mount the volume
  //
  // 1. get volume identifier (from ENVIRONMENT from task in ExecutorInfo
  //     VOL_NAME_ENV_VAR_NAME is defined below
  //     This is volume name, not ID.
  //     Warning, name collisions on name can be treacherous.
  //     For now a simple string value is presumed, will need to enhance to
  //     support a JSON array to allow multiple volume mounts per task.
  // 2. get desired volume driver (volumedriver=) from ENVIRONMENT from task in ExecutorInfo
  //     VOL_DRIVER_ENV_VAR_NAME is defined below
  // 3. Check for other pre-existing users of the mount.
  // 4. Only if we are first user, make dvdcli mount call <volumename>
  //    Mount location is fixed, based on volume name (/var/lib/rexray/volumes/
  //    this call is synchronous, and returns 0 if success
  //    actual call is defined below in DVDCLI_MOUNT_CMD
  // 5. Add entry to hashmap that contains root mountpath indexed by ContainerId
  virtual process::Future<Option<ContainerPrepareInfo>> prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const std::string& directory,
    const Option<std::string>& user);

  // Nothing will be done at task start
  virtual process::Future<Nothing> isolate(
    const ContainerID& containerId,
      pid_t pid);

  // no-op, mount occurs at prepare
  virtual process::Future<ContainerLimitation> watch(
    const ContainerID& containerId);

  // no-op, nothing enforced
  virtual process::Future<Nothing> update(
    const ContainerID& containerId,
    const Resources& resources);

  // no-op, no usage stats gathered
  virtual process::Future<ResourceStatistics> usage(
    const ContainerID& containerId);

  // will (possibly) unmount here
  // 1. Get mount root path by looking up based on ContainerId
  // 2. Start counting tasks using this same mount. Quit counted after count == 2
  // 3. If count was exactly 1, Unmount the volume
  //     dvdcli unmount defined in DVDCLI_UNMOUNT_CMD below
  // 4. Remove the listing for this task's mount from hashmap
  virtual process::Future<Nothing> cleanup(
    const ContainerID& containerId);

private:
  DockerVolumeDriverIsolator(const Parameters& parameters);

  const Parameters parameters;

  using ExternalMountID = size_t;

  ExternalMountID getExternalMountId(ExternalMount& em) const {
    size_t seed = 0;
    std::string s1(boost::to_lower_copy(em.volumedriver()));
    std::string s2(boost::to_lower_copy(em.volumename()));
    boost::hash_combine(seed, s1);
    boost::hash_combine(seed, s2);
    return seed;
  }

  // Attempts to unmount specified external mount, returns true on success
  bool unmount(
    const ExternalMount& em,
    const std::string&   callerLabelForLogging) const;

  // Attempts to mount specified external mount,
  // returns non-empty string on success
  std::string mount(
    const ExternalMount& em,
    const std::string&   callerLabelForLogging) const;

  // Returns true if string contains at least one prohibited character
  // as defined in the list below.
  // This is intended as a tool to detect injection attack attempts.
  bool containsProhibitedChars(const std::string& s) const;

  using containermountmap =
    multihashmap<ContainerID, process::Owned<ExternalMount>>;
  containermountmap infos;

  // compiler had issues with the autodetecting size of following array,
  // thus a constant is defined

  static constexpr size_t NUM_PROHIBITED = 26;
  static const char prohibitedchars[NUM_PROHIBITED]; /*  = {
  '%', '/', ':', ';', '\0',
  '<', '>', '|', '`', '$', '\'',
  '?', '^', '&', ' ', '{', '\"',
  '}', '[', ']', '\n', '\t', '\v', '\b', '\r', '\\' };*/

  static std::string mountPbFilename;
  static std::string mesosWorkingDir;
};

} /* namespace slave */
} /* namespace mesos */

#endif /* SRC_DOCKER_VOLUME_DRIVER_ISOLATOR_HPP_ */
