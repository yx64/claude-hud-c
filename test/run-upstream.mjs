// Runs one upstream claude-hud refresh the way its own launcher (scripts/statusline.mjs) does,
// from the directory given on the command line instead of the newest plugin install. Usage:
//   node --import ./fakeclock.mjs run-upstream.mjs <UpstreamDir>
import path from 'node:path';
import { pathToFileURL } from 'node:url';

// Claude Code pads the status line by 2 columns on each side.
const columns = Number.parseInt(process.env.COLUMNS ?? '', 10);
if (columns > 0) process.env.COLUMNS = String(Math.max(1, columns - 4));

const hud = await import(pathToFileURL(path.join(process.argv[2], 'dist', 'index.js')).href);
await hud.main();
