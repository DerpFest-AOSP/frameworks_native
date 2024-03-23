# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_DIR := $(GET_LOCAL_DIR)
LIBBINDER_DIR := $(LOCAL_DIR)/../..

MODULE := $(LOCAL_DIR)

MODULE_SRCS := $(LIBBINDER_DIR)/rust/src/lib.rs

MODULE_CRATE_NAME := binder

MODULE_LIBRARY_DEPS += \
	$(LIBBINDER_DIR)/trusty \
	$(LIBBINDER_DIR)/trusty/ndk \
	$(LIBBINDER_DIR)/trusty/rust/binder_ndk_sys \
	$(LIBBINDER_DIR)/trusty/rust/binder_rpc_unstable_bindgen \
	external/rust/crates/downcast-rs \
	trusty/user/base/lib/trusty-sys \

# Trusty does not have `ProcessState`, so there are a few
# doc links in `IBinder` that are still broken.
MODULE_RUSTFLAGS += \
	--allow rustdoc::broken-intra-doc-links \

include make/library.mk
