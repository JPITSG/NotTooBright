const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('../assets/node_modules/typescript');

function loadTs(relative, globals = {}) {
  const source = fs.readFileSync(path.join(__dirname, '..', relative), 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  const exports = {};
  vm.runInNewContext(output, { exports, Date, Math, Set, ...globals });
  return exports;
}

module.exports = { loadTs };
