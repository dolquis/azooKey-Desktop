export const meta = {
  name: 'pre-pr-gate',
  description: 'PR 前の L2: 親が route で選んだ specialist を並列起動し、P1/P2 の指摘を独立に検証する',
  whenToUse: '編集を止めた checkpoint で、diff-auditor / spec-drift-checker / quality-gate-runner / windows-build-runner のうち 2 体以上を回すとき。route の判定と args の組み立ては親が行う（docs/handoff/agent-orchestration.md）',
  phases: [
    { title: 'Gate', detail: 'specialist を並列起動（最大 4 体、build runner 以外は read-only）' },
    { title: 'Verify', detail: 'reviewer の P1/P2 を diff-auditor が反証の立場で検証（3 体ずつ）' },
  ],
}

// args の形（親が inline で組み立てる。route は Workflow の中では判定できない）:
// {
//   issue: 'DEV-xxxx',
//   base: '<merge-base SHA>',
//   head: '<commit SHA または "HEAD + 作業ツリー">',
//   touchedPaths: ['tsf-tip/src/...', ...],
//   boundary: 'tsf | ipc | conversion | learning-privacy | (none)',
//   agents: ['diff-auditor', 'spec-drift-checker', 'quality-gate-runner', 'windows-build-runner'],
//   outOfScope: '<paths / decisions>',           // 任意
//   build: { preset: 'windows-debug', target: 'azookey_check', tests: '-R ...' },  // windows-build-runner を含むとき必須
// }

const ALLOWED = ['diff-auditor', 'spec-drift-checker', 'quality-gate-runner', 'windows-build-runner']
const REVIEWERS = ['diff-auditor', 'spec-drift-checker']

if (!args || typeof args !== 'object') {
  throw new Error('args が無い。親が base / head / touchedPaths / agents を渡す（docs/handoff/agent-orchestration.md「snapshot 契約」）')
}
for (const key of ['issue', 'base', 'head', 'boundary']) {
  if (typeof args[key] !== 'string' || !args[key].trim()) throw new Error(`args.${key} が無い`)
}
if (!Array.isArray(args.touchedPaths) || args.touchedPaths.length === 0) throw new Error('args.touchedPaths が空')
if (!Array.isArray(args.agents) || args.agents.length === 0) throw new Error('args.agents が空')
// 1 体だけなら Workflow を使わず、親がその agent を直接起動する（L1。orchestration「分割レベル」）
if (args.agents.length < 2) throw new Error('args.agents が 1 体だけ。L2 の Workflow は 2 体以上で使い、1 体なら親が直接起動する')
const unknown = args.agents.filter(a => !ALLOWED.includes(a))
if (new Set(args.agents).size !== args.agents.length) throw new Error('args.agents に重複がある')
if (unknown.length) throw new Error(`args.agents に対象外の agent: ${unknown.join(', ')}`)
// 同時起動は横断変更でも specialist 3 体 + build runner 1 枠まで（orchestration「管制塔セッションと子セッション」）
const nonBuild = args.agents.filter(a => a !== 'windows-build-runner')
if (nonBuild.length > 3) throw new Error('build runner 以外の specialist は 3 体まで')
if (args.agents.includes('windows-build-runner') && !(args.build && args.build.preset)) {
  throw new Error('windows-build-runner を使うときは args.build.preset が必要（build directory を共有しないため）')
}

const RETURN_SCHEMA = {
  type: 'object',
  properties: {
    snapshot: { type: 'string', description: 'Base、Review head、Boundary と確認範囲' },
    findings: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          severity: { type: 'string', enum: ['P1', 'P2', 'P3'] },
          location: { type: 'string', description: 'file / symbol' },
          evidence: { type: 'string' },
          impact: { type: 'string' },
          recommendation: { type: 'string' },
          confirmed: { type: 'boolean', description: 'true は確認済み、false は推測' },
        },
        required: ['severity', 'location', 'evidence', 'impact', 'recommendation', 'confirmed'],
      },
    },
    commands: { type: 'array', items: { type: 'string' }, description: '実行したコマンドと終了コード、またはログの位置' },
    notCovered: { type: 'array', items: { type: 'string' }, description: '確認できなかった範囲と理由' },
    nextChecks: { type: 'array', items: { type: 'string' }, description: '親が次に実行すべき検証' },
  },
  required: ['snapshot', 'findings', 'commands', 'notCovered', 'nextChecks'],
}

const VERDICT_SCHEMA = {
  type: 'object',
  properties: {
    refuted: { type: 'boolean' },
    reason: { type: 'string' },
  },
  required: ['refuted', 'reason'],
}

function snapshot(agentName, forVerify) {
  const writeBoundary = agentName === 'windows-build-runner'
    ? `build directory のみ（preset ${args.build.preset}）`
    : 'read-only'
  const lines = [
    `Issue: ${args.issue}`,
    `Base: ${args.base}`,
    `Review head: ${args.head}`,
    `Scope: ${args.touchedPaths.join(', ')}`,
    `Out of scope: ${args.outOfScope || '(none)'}`,
    `Boundary: ${args.boundary}`,
    `Write boundary: ${writeBoundary}`,
    'Spawn: forbidden',
    forVerify
      ? 'Expected return: refuted と reason だけを返す'
      : 'Expected return: docs/handoff/agent-orchestration.md「統一返却形式」を返却 schema の各欄に入れる。該当なしでも確認範囲を snapshot と notCovered に書く',
    'Validation owned by parent: commit、push、PR、Linear、最終判定',
  ]
  if (agentName === 'windows-build-runner') {
    lines.push(`Build: preset ${args.build.preset}${args.build.target ? `, target ${args.build.target}` : ''}${args.build.tests ? `, tests ${args.build.tests}` : ''}`)
    lines.push('失敗した target / CTest 名は findings に、各フェーズの終了コードは commands に入れる')
  }
  if (agentName === 'quality-gate-runner') {
    lines.push('Scope の変更範囲から、定義の表に従ってゲートを選ぶ。失敗したゲートは findings に入れる')
  }
  return lines.join('\n')
}

phase('Gate')
log(`specialist: ${args.agents.join(', ')}`)
// parallel は失敗した thunk を null で返すので、agent 名は index で対応付け、null も failed として残す。
const raw = await parallel(args.agents.map(agentName => () =>
  agent(snapshot(agentName, false), {
    label: agentName,
    phase: 'Gate',
    agentType: agentName,
    schema: RETURN_SCHEMA,
  })
))
const results = args.agents.map((agentName, i) => {
  const r = raw[i]
  if (!r || !Array.isArray(r.findings)) return { agent: agentName, failed: true }
  return { agent: agentName, ...r }
})

// 反証検証は reviewer の P1/P2 だけ。runner の失敗は終了コードが証拠なので検証しない。
const toVerify = []
for (const r of results) {
  if (r.failed || !REVIEWERS.includes(r.agent)) continue
  for (const f of r.findings) {
    if (f.severity === 'P1' || f.severity === 'P2') toVerify.push({ from: r.agent, finding: f })
  }
}
const p3 = results.filter(r => !r.failed && REVIEWERS.includes(r.agent))
  .reduce((n, r) => n + r.findings.filter(f => f.severity === 'P3').length, 0)
if (p3) log(`P3 の ${p3} 件は検証せず、そのまま返す`)

phase('Verify')
const verdicts = []
for (let i = 0; i < toVerify.length; i += 3) {
  const chunk = toVerify.slice(i, i + 3)
  const out = await parallel(chunk.map((item, j) => () =>
    agent([
      snapshot('diff-auditor', true),
      '',
      `次の指摘（${item.from} が報告）を反証する立場で検証する。差分と正典を自分で読み、根拠が成り立たなければ refuted=true とする。判断がつかなければ refuted=false とし、reason に確認できなかった点を書く。`,
      JSON.stringify(item.finding, null, 2),
    ].join('\n'), {
      label: `verify:${item.finding.location}`.slice(0, 60),
      phase: 'Verify',
      agentType: 'diff-auditor',
      schema: VERDICT_SCHEMA,
    })
  ))
  // 検証が失敗した指摘も落とさず unverified に残す。
  chunk.forEach((item, j) => verdicts.push({ ...item, verdict: out[j] || null }))
}

const failedAgents = results.filter(r => r.failed).map(r => r.agent)
if (failedAgents.length) log(`結果を返さなかった agent: ${failedAgents.join(', ')}。親が同じ範囲を担当する（parent-only fallback）`)

return {
  snapshot: { issue: args.issue, base: args.base, head: args.head, boundary: args.boundary },
  results,
  verified: verdicts.filter(v => v.verdict && !v.verdict.refuted),
  refuted: verdicts.filter(v => v.verdict && v.verdict.refuted),
  unverified: verdicts.filter(v => !v.verdict),
  failedAgents,
}
