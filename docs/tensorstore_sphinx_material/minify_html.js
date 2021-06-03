const minhtml = require('html-minifier').minify;
const fs = require('fs').promises;

if (!process.env.HOME) {
  // When invoked by Bazel, HOME may be unset which can cause problems with
  // uv_os_homedir.
  process.env.HOME = process.cwd();
}

async function main() {
  const sourcesAndOutputs = process.argv.slice(2);
  if (sourcesAndOutputs.length % 2 !== 0) {
    console.log('Expected an even number of arguments');
    process.exit(1);
  }

  const numInputs = sourcesAndOutputs.length / 2;
  for (let i = 0; i < numInputs; ++i) {
    const inputPath = sourcesAndOutputs[i];
    const outputPath = sourcesAndOutputs[i + numInputs];

    const data = await fs.readFile(inputPath, 'utf8');
    let html = data.replace(/\r\n/gm, '\n');
    html = minhtml(html, {
             collapseBooleanAttributes: true,
             includeAutoGeneratedTags: false,
             collapseWhitespace: true,
             minifyCSS: true,
             minifyJS: true,
             removeComments: true,
             removeScriptTypeAttributes: true,
             removeStyleLinkTypeAttributes: true
           })
               .replace(
                   /* Remove empty lines without collapsing everything */
                   /^\s*[\r\n]/gm, '');
    await fs.writeFile(outputPath, html);
  }
}

main();