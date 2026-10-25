// Copyright (c) 2026 foggy (abit-foggy)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

use zed_extension_api::{self as zed, LanguageServerConfiguration, Result};

struct ZovExtension;

impl zed::Extension for ZovExtension {
    fn new() -> Self {
        ZovExtension
    }

    // The grammar is bundled with the extension manifest; no repo fetch.
    fn language_server_command(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
        _project_root: &std::path::Path,
    ) -> Result<zed::Command> {
        // zov-lsp is resolved from $PATH or the workspace root; Zed launches
        // it over stdio, matching the server's fd 0/1 JSON-RPC transport.
        let lsp = which_zov_lsp()
            .unwrap_or_else(|| "zov-lsp".to_string());
        Ok(zed::Command {
            command: lsp,
            args: vec![],
            env: vec![],
        })
    }

    fn language_server_initialization_options(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
    ) -> Result<Option<zed::serde_json::Value>> {
        Ok(None)
    }

    fn language_server_workspace_configuration(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
        _project_configuration: Option<zed::serde_json::Value>,
    ) -> Result<Option<zed::serde_json::Value>> {
        Ok(None)
    }

    fn language_configs(&mut self) -> Result<Vec<zed::LanguageConfig>> {
        Ok(vec![zed::LanguageConfig {
            name: "Zov".into(),
            grammar: Some("zov".into()),
            line_comments: vec!["// ".into()],
            path_suffixes: vec!["zov".into()],
            ..Default::default()
        }])
    }

    fn language_server_configurations(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
    ) -> Result<Vec<LanguageServerConfiguration>> {
        Ok(vec![])
    }
}

fn which_zov_lsp() -> Option<String> {
    // PATH lookup first; fall back to the workspace-local binary produced
    // by `make zov-lsp` at the zov repository root.
    if let Ok(path) = std::env::var("PATH") {
        for dir in path.split(':') {
            if dir.is_empty() {
                continue;
            }
            let candidate = std::path::Path::new(dir).join("zov-lsp");
            if candidate.is_file() {
                return Some(candidate.to_string_lossy().into_owned());
            }
        }
    }
    None
}

zed::register_extension!(ZovExtension);
