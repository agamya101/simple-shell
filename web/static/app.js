// ── Terminal setup ─────────────────────────────────────────────────────────
const term = new Terminal({
  cursorBlink: true,
  fontFamily: '"JetBrains Mono", "Fira Code", Menlo, Consolas, monospace',
  fontSize: 14,
  theme: {
    background: "#0d1117",
    foreground: "#e6edf3",
    cursor: "#58a6ff",
    selectionBackground: "#264f78",
  },
});
const fitAddon = new FitAddon.FitAddon();
term.loadAddon(fitAddon);
term.open(document.getElementById("terminal"));
fitAddon.fit();

let ws = null;
let rootDir = "";

const statusEl = document.getElementById("status");
const statusText = document.getElementById("status-text");

function setStatus(state, text) {
  statusEl.className = "status " + state;
  statusText.textContent = text;
}

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

function sendInput(text) {
  send({ type: "input", data: text });
  term.focus();
}

function sendResize() {
  send({ type: "resize", rows: term.rows, cols: term.cols });
}

// ── WebSocket connection ───────────────────────────────────────────────────
function connect() {
  term.reset();
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.binaryType = "arraybuffer";

  ws.onopen = () => {
    setStatus("on", "connected");
    fitAddon.fit();
    sendResize();
    term.focus();
  };

  ws.onmessage = (event) => {
    if (typeof event.data !== "string") {
      term.write(new Uint8Array(event.data));
      return;
    }
    const msg = JSON.parse(event.data);
    if (msg.type === "hello") {
      rootDir = msg.cwd;
      renderSteps();
    } else if (msg.type === "procs") {
      renderProcs(msg.shell, msg.procs);
    } else if (msg.type === "exit") {
      term.write("\r\n\x1b[90m[shell exited — click \"Restart shell\" to start a new one]\x1b[0m\r\n");
    }
  };

  ws.onclose = () => {
    setStatus("off", "disconnected");
    renderProcs(null, []);
  };
}

term.onData((data) => send({ type: "input", data }));

new ResizeObserver(() => {
  fitAddon.fit();
  sendResize();
}).observe(document.getElementById("terminal"));

document.getElementById("btn-ctrlc").onclick = () => sendInput("\x03");
document.getElementById("btn-ctrlz").onclick = () => sendInput("\x1a");
document.getElementById("btn-restart").onclick = () => {
  if (ws) ws.close();
  setTimeout(connect, 150);
};

// ── Live process table ─────────────────────────────────────────────────────
const STATE_NAMES = {
  R: "running", S: "sleeping", D: "disk wait", T: "stopped", t: "traced", Z: "zombie",
};

function stateClass(s) {
  if (s === "T" || s === "t") return "st-stop";
  if (s === "Z") return "st-zombie";
  return "st-run";
}

function escapeHtml(s) {
  return s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

function renderProcs(shellPid, procs) {
  const body = document.getElementById("procs");
  const owner = document.getElementById("owner");
  if (!procs.length) {
    body.innerHTML = `<tr><td colspan="4" class="muted">${shellPid === null ? "not connected" : "no process data (needs Linux /proc)"}</td></tr>`;
    owner.textContent = "Terminal owner: —";
    return;
  }
  const tpgid = procs[0].tpgid;
  const ownerProc = procs.find((p) => p.pgid === tpgid);
  owner.textContent = `Terminal owner: PGID ${tpgid}` + (ownerProc ? ` (${ownerProc.comm})` : "");

  body.innerHTML = procs.map((p) => {
    const fg = p.pgid === tpgid;
    const indent = "&nbsp;&nbsp;".repeat(p.depth) + (p.depth ? "└ " : "");
    return `<tr class="${fg ? "fg" : ""}">
      <td>${p.pid}</td>
      <td>${p.pgid}${fg ? '<span class="fg-badge">FG</span>' : ""}</td>
      <td><span class="st ${stateClass(p.state)}" title="${STATE_NAMES[p.state] || p.state}">${p.state}</span></td>
      <td class="cmd" title="${escapeHtml(p.cmd)}">${indent}${escapeHtml(p.cmd)}</td>
    </tr>`;
  }).join("");
}

// ── Demo steps with explanations ───────────────────────────────────────────
// Each step is either a command (typed + Enter) or a keystroke.
function demoGroups() {
  return [
    {
      title: "1 · Built-in commands",
      steps: [
        { cmd: "pwd", info: "Built-in: the shell calls <code>getcwd()</code> itself — no fork." },
        { cmd: "cd /tmp", info: "<code>cd</code> must be a built-in: the working directory belongs to a process. A child running <code>chdir()</code> would change only its own directory, then exit." },
        { cmd: `cd ${rootDir}`, info: "Back to the project folder. Note the prompt shows the current folder name, coloured with ANSI escape codes." },
        { cmd: "echo hello world", info: "Built-in <code>echo</code>: prints its arguments, run inside the shell process." },
        { cmd: "help", info: "Lists the built-ins and features." },
      ],
    },
    {
      title: "2 · External commands (fork → exec → wait)",
      steps: [
        { cmd: "ls -l", info: "<code>fork()</code> makes a child; the child calls <code>execvp(\"ls\")</code> to become <code>ls</code>; the parent waits with <code>waitpid()</code>." },
        { cmd: "whoami", info: "Same fork–exec–wait flow. <code>execvp</code> searches the <code>PATH</code> for the program." },
        { cmd: "abcxyz", info: "<code>execvp</code> fails and returns, so the child prints an error and calls <code>exit(1)</code>. The shell is unaffected." },
      ],
    },
    {
      title: "3 · I/O redirection",
      steps: [
        { cmd: "ls > out.txt", info: "<code>open()</code> the file, then <code>dup2(fd, STDOUT_FILENO)</code> in the child before exec. <code>ls</code> writes to fd 1 without knowing it is a file." },
        { cmd: "cat out.txt", info: "Shows the file that <code>ls</code> wrote." },
        { cmd: "wc -l < out.txt", info: "Input redirection: <code>dup2(fd, STDIN_FILENO)</code>, so <code>wc</code> reads the file as its stdin." },
      ],
    },
    {
      title: "4 · Pipe",
      steps: [
        { cmd: "ls | grep cpp", info: "<code>pipe()</code> creates a kernel buffer. Left child: stdout → write end. Right child: stdin ← read end. Every process closes unused ends so <code>grep</code> sees EOF." },
      ],
    },
    {
      title: "5 · Signals: Ctrl+C",
      steps: [
        { cmd: "sleep 30", info: "Watch the process panel: <code>sleep</code> has its own PGID and is marked <b>FG</b> — the shell gave it the terminal with <code>tcsetpgrp()</code>." },
        { key: "\x03", label: "Ctrl+C", info: "The terminal sends SIGINT to the foreground process group. The shell ignores SIGINT; the child restored <code>SIG_DFL</code> before exec, so only <code>sleep</code> dies." },
      ],
    },
    {
      title: "6 · Job control",
      steps: [
        { cmd: "sleep 100 &", info: "Background job: own process group, parent does <b>not</b> wait. The shell keeps the terminal (FG stays on myshell)." },
        { cmd: "sleep 200", info: "Foreground job — it now owns the terminal (FG badge)." },
        { key: "\x1a", label: "Ctrl+Z", info: "SIGTSTP stops the foreground group. State becomes <b>T</b>. <code>waitpid(WUNTRACED)</code> returns with <code>WIFSTOPPED</code>, the shell takes the terminal back." },
        { cmd: "jobs", info: "Built-in: prints the job table (<code>map&lt;int, Job&gt;</code>)." },
        { cmd: "bg 2", info: "<code>kill(-pgid, SIGCONT)</code> resumes job 2 — but without giving it the terminal. State T → S." },
        { cmd: "fg 1", info: "SIGCONT + <code>tcsetpgrp()</code> + wait: job 1 is foreground again (FG badge moves)." },
        { key: "\x03", label: "Ctrl+C", info: "Kill job 1." },
        { cmd: "fg 2", info: "Bring job 2 to the foreground." },
        { key: "\x03", label: "Ctrl+C", info: "Kill job 2." },
        { cmd: "jobs", info: "Job table is empty again." },
      ],
    },
    {
      title: "7 · Reaping background jobs",
      steps: [
        { cmd: "sleep 3 &", info: "Short background job. When it ends it becomes a <b>zombie (Z)</b> in the process panel until the parent collects it." },
        { key: "\r", label: "Enter (after 3s)", info: "Before each prompt <code>reapJobs()</code> calls <code>waitpid(WNOHANG)</code>: the zombie is reaped and \"Done\" is printed." },
      ],
    },
    {
      title: "8 · History",
      steps: [
        { cmd: "echo testing", info: "Every command is pushed into a <code>vector&lt;string&gt;</code>." },
        { cmd: "!!", info: "<code>!!</code> repeats the last command from history." },
        { cmd: "history", info: "Prints the history. It is saved to <code>~/.myshell_history</code> on <code>exit</code> and loaded at startup." },
      ],
    },
  ];
}

const explainBox = document.getElementById("explain");
document.getElementById("explain-close").onclick = () => explainBox.classList.add("hidden");

function explain(title, html) {
  document.getElementById("explain-title").textContent = title;
  document.getElementById("explain-body").innerHTML = html;
  explainBox.classList.remove("hidden");
}

function renderSteps() {
  const container = document.getElementById("steps");
  container.innerHTML = "";
  demoGroups().forEach((group) => {
    const g = document.createElement("div");
    g.className = "group";
    g.innerHTML = `<h3>${group.title}</h3>`;
    group.steps.forEach((step) => {
      const btn = document.createElement("button");
      btn.className = "step";
      const label = step.key ? step.label : step.cmd;
      btn.innerHTML = `<span class="step-cmd ${step.key ? "keystroke" : ""}">${escapeHtml(label)}</span><span class="step-hint">${step.key ? "key" : "run"}</span>`;
      btn.onclick = () => {
        sendInput(step.key ? step.key : step.cmd + "\r");
        btn.classList.add("done");
        explain(label, step.info);
      };
      g.appendChild(btn);
    });
    container.appendChild(g);
  });
}

renderSteps();
connect();
