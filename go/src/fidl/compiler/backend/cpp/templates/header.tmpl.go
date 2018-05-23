// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const Header = `
{{- define "GenerateHeaderPreamble" -}}
// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// WARNING: This file is machine generated by fidlgen.

#pragma once

#include "lib/fidl/cpp/internal/header.h"

{{ range .Headers -}}
#include <{{ . }}>
{{ end -}}

{{- range .Library }}
namespace {{ . }} {
{{- end }}
{{ end }}

{{- define "GenerateHeaderPostamble" -}}
{{- range .Library }}
}
{{- end }}
{{ end }}

{{- define "GenerateTraitsPreamble" -}}
namespace fidl {
{{ end }}

{{- define "GenerateTraitsPostamble" -}}
}  // namespace fidl
{{ end }}
`
