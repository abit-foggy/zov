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

'use strict';

const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client = null;

function serverOptions() {
    const configured = vscode.workspace.getConfiguration('zov.lsp').get('path');
    return {
        command: configured || 'zov-lsp',
        transport: TransportKind.stdio
    };
}

function clientOptions() {
    return {
        documentSelector: [{ language: 'zov', scheme: 'file' }],
        outputChannelName: 'zov-lsp'
    };
}

function activate(context) {
    client = new LanguageClient(
        'zov-lsp',
        'Zov Language Server',
        serverOptions(),
        clientOptions()
    );
    context.subscriptions.push(
        vscode.commands.registerCommand('zov.restartLsp', async () => {
            if (client) {
                await client.stop();
            }
            client = new LanguageClient(
                'zov-lsp',
                'Zov Language Server',
                serverOptions(),
                clientOptions()
            );
            await client.start();
        })
    );
    context.subscriptions.push({
        dispose: () => {
            if (client) {
                return client.stop();
            }
            return undefined;
        }
    });
    client.start();
}

function deactivate() {
    if (client) {
        return client.stop();
    }
    return undefined;
}

module.exports = { activate, deactivate };
