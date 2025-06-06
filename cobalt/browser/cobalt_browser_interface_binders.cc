// Copyright 2025 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cobalt/browser/cobalt_browser_interface_binders.h"

#include "base/functional/bind.h"
#include "cobalt/browser/crash_annotator/public/mojom/crash_annotator.mojom.h"
#include "cobalt/browser/h5vcc_accessibility/h5vcc_accessibility_impl.h"
#include "cobalt/browser/h5vcc_accessibility/public/mojom/h5vcc_accessibility.mojom.h"
#include "cobalt/browser/h5vcc_experiments/h5vcc_experiments_impl.h"
#include "cobalt/browser/h5vcc_experiments/public/mojom/h5vcc_experiments.mojom.h"
#include "cobalt/browser/h5vcc_metrics/h5vcc_metrics_impl.h"
#include "cobalt/browser/h5vcc_metrics/public/mojom/h5vcc_metrics.mojom.h"
#include "cobalt/browser/h5vcc_runtime/h5vcc_runtime_impl.h"
#include "cobalt/browser/h5vcc_runtime/public/mojom/h5vcc_runtime.mojom.h"
#include "cobalt/browser/h5vcc_system/h5vcc_system_impl.h"
#include "cobalt/browser/h5vcc_system/public/mojom/h5vcc_system.mojom.h"
#include "cobalt/browser/performance/performance_impl.h"
#include "cobalt/browser/performance/public/mojom/performance.mojom.h"

#if BUILDFLAG(IS_ANDROIDTV)
#include "content/public/browser/render_frame_host.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#else
#include "cobalt/browser/crash_annotator/crash_annotator_impl.h"
#endif  // BUILDFLAG(IS_ANDROIDTV)

namespace cobalt {

#if BUILDFLAG(IS_ANDROIDTV)
template <typename Interface>
void ForwardToJavaFrame(content::RenderFrameHost* render_frame_host,
                        mojo::PendingReceiver<Interface> receiver) {
  render_frame_host->GetJavaInterfaces()->GetInterface(std::move(receiver));
}
#endif  // BUILDFLAG(IS_ANDROIDTV)

void PopulateCobaltFrameBinders(
    content::RenderFrameHost* render_frame_host,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* binder_map) {
// We want to use the Java Mojo implementation for 1P ATV only.
#if BUILDFLAG(IS_ANDROIDTV)
  binder_map->Add<crash_annotator::mojom::CrashAnnotator>(base::BindRepeating(
      &ForwardToJavaFrame<crash_annotator::mojom::CrashAnnotator>));
#else
  binder_map->Add<crash_annotator::mojom::CrashAnnotator>(
      base::BindRepeating(&crash_annotator::CrashAnnotatorImpl::Create));
#endif  // BUILDFLAG(IS_ANDROIDTV)
  binder_map->Add<h5vcc_accessibility::mojom::H5vccAccessibilityBrowser>(
      base::BindRepeating(
          &h5vcc_accessibility::H5vccAccessibilityImpl::Create));
  binder_map->Add<h5vcc_experiments::mojom::H5vccExperiments>(
      base::BindRepeating(&h5vcc_experiments::H5vccExperimentsImpl::Create));
  binder_map->Add<h5vcc_metrics::mojom::H5vccMetrics>(
      base::BindRepeating(&h5vcc_metrics::H5vccMetricsImpl::Create));
  binder_map->Add<h5vcc_system::mojom::H5vccSystem>(
      base::BindRepeating(&h5vcc_system::H5vccSystemImpl::Create));
  binder_map->Add<h5vcc_runtime::mojom::H5vccRuntime>(
      base::BindRepeating(&h5vcc_runtime::H5vccRuntimeImpl::Create));
  binder_map->Add<performance::mojom::CobaltPerformance>(
      base::BindRepeating(&performance::PerformanceImpl::Create));
}

}  // namespace cobalt
