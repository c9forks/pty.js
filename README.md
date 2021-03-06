# pty.js

**⚠️ This project will soon be published to npm under a new name, in the meantime [c75c2dc](https://github.com/Tyriar/pty.js/commit/c75c2dcb6dcad83b0cb3ef2ae42d0448fb912642) is the stable version that should be used, *not* the `HEAD` of `master`. See [this issue](https://github.com/Microsoft/vscode/issues/13625) for more details.**

`forkpty(3)` bindings for node.js. This allows you to fork processes with pseudo
terminal file descriptors. It returns a terminal object which allows reads
and writes.

This is useful for:

- Writing a terminal emulator.
- Getting certain programs to *think* you're a terminal. This is useful if
  you need a program to send you control sequences.

## Example Usage

``` js
var pty = require('pty.js');

var term = pty.spawn('bash', [], {
  name: 'xterm-color',
  cols: 80,
  rows: 30,
  cwd: process.env.HOME,
  env: process.env
});

term.on('data', function(data) {
  console.log(data);
});

term.write('ls\r');
term.resize(100, 40);
term.write('ls /\r');

console.log(term.process);
```

## Todo

- Add tcsetattr(3), tcgetattr(3).
- Add a way of determining the current foreground job for platforms other
  than Linux and OSX/Darwin.

## Contribution and License Agreement

If you contribute code to this project, you are implicitly allowing your code
to be distributed under the MIT license. You are also implicitly verifying that
all code is your original work. `</legalese>`

## License

Copyright (c) 2012-2015, Christopher Jeffrey (MIT License).
