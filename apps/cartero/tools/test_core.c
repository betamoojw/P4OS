/* Cartero: the core (values, JSON, the interpreter, pm) tried on the Mac. Built and run by test_core.sh. */
#include "ct.h"
#include <stdio.h>
#include <stdlib.h>
static int fails = 0;
static const char *BODY = "{\"data\":[{\"id\":7,\"name\":\"Ana\",\"active\":true,\"tags\":[\"a\",\"b\"]},{\"id\":8,\"name\":\"Bruno\",\"active\":false}],\"page\":1,\"total\":12,\"nested\":{\"x\":{\"y\":[1,2,3]}}}";
static void run(const char *name, const char *src, int want_pass, int want_fail, const char *want_err)
{
    ct_arena_t ar; ct_arena_init(&ar, 8<<20);
    ct_script_t c; memset(&c, 0, sizeof c);
    c.ar = &ar;
    c.env = ct_obj_new(&ar, 0); ct_sets(&ar, c.env, "base", "http://x"); ct_sets(&ar, c.env, "per_page", "5");
    c.glob = ct_obj_new(&ar, 0); c.coll = ct_obj_new(&ar, 0); c.local = ct_obj_new(&ar, 0);
    c.req = ct_obj_new(&ar, 0); ct_sets(&ar, c.req, "method", "GET"); ct_sets(&ar, c.req, "url", "http://x/users?page=1");
    ct_setv(&ar, c.req, "header", ct_arr_new(&ar, 0));
    ct_resp_t r; memset(&r, 0, sizeof r);
    r.ok = 1; r.status = 200; strcpy(r.reason, "OK"); r.body = (uint8_t*)BODY; r.len = strlen(BODY); r.t_total = 42;
    r.hdr[0].k = "Content-Type"; r.hdr[0].v = "application/json; charset=utf-8"; r.hdr[1].k="Set-Cookie"; r.hdr[1].v="sid=abc; Path=/"; r.nhdr = 2;
    c.resp = &r;
    ct_sb_t con = {0}; c.console = &con;
    c.request_name = "List"; c.event = "test";
    bool ok = ct_js_run(&c, src);
    int pass = 0, fail = 0;
    for (uint32_t i = 0; i < ct_len(c.tests); i++) { if (ct_truthy(ct_get(ct_at(c.tests,i),"ok"))) pass++; else fail++; }
    bool good = (want_err ? (!ok && strstr(c.error, want_err)) : ok) && pass == want_pass && fail == want_fail;
    if (!good) fails++;
    printf("%s %-28s pass=%d fail=%d %s%s\n", good ? "ok  " : "FAIL", name, pass, fail, ok ? "" : "err=", ok ? "" : c.error);
    for (uint32_t i = 0; i < ct_len(c.tests); i++) { ctv_t t = ct_at(c.tests,i); if (!ct_truthy(ct_get(t,"ok")) || !good) printf("      - %s: %s\n", ct_s(ct_get(t,"name")), ct_s(ct_get(t,"error"))); }
    if (con.n && (!good || getenv("V"))) printf("      console: %s", con.p);
    if (c.changes.t == CT_ARR && getenv("V")) { ct_sb_t b={0}; ct_json_write(&b, c.changes, 0); printf("      changes: %s\n", b.p); ct_sb_free(&b);} 
    ct_sb_free(&con); ct_arena_free(&ar);
}
int main(void)
{
    run("basic", "pm.test('200', () => pm.response.to.have.status(200)); pm.test('bad', function(){ pm.expect(1+1).to.equal(3) });", 1, 1, NULL);
    run("json", "const j = pm.response.json(); pm.test('a', () => { pm.expect(j.data).to.be.an('array'); pm.expect(j.data.length).to.equal(2); pm.expect(j.data[0].name).to.eql('Ana'); pm.expect(j.data[1].active).to.be.false; pm.expect(j).to.have.property('total', 12); });", 1, 0, NULL);
    run("env", "pm.environment.set('first_id', pm.response.json().data[0].id); pm.test('x', () => pm.expect(pm.environment.get('first_id')).to.equal(7)); pm.test('v', ()=> pm.expect(Number(pm.variables.get('per_page'))).to.equal(5));", 2, 0, NULL);
    run("closures/loops", "let s=0; for (let i=0;i<10;i++){ if(i%2) continue; s+=i; } const fns=[]; for (let i=0;i<3;i++) fns.push(()=>i); pm.test('s', ()=>pm.expect(s).to.equal(20)); pm.test('c', ()=>pm.expect(fns.map(f=>f())).to.eql([0,1,2]));", 2, 0, NULL);
    run("strings", "const t = `id=${pm.response.json().data[0].id} n=${'x'.repeat(3)}`; pm.test('t', ()=>pm.expect(t).to.equal('id=7 n=xxx')); pm.test('s', ()=>{ pm.expect('a,b,c'.split(',')).to.have.lengthOf(3); pm.expect(' hi '.trim().toUpperCase()).to.equal('HI'); pm.expect('hello'.slice(-3)).to.equal('llo'); pm.expect('abc'.includes('b')).to.be.true; pm.expect('5'.padStart(3,'0')).to.equal('005'); });", 2, 0, NULL);
    run("arrays", "const a=[3,1,2]; a.sort(); const r = a.reduce((x,y)=>x+y, 0); pm.test('x', ()=>{ pm.expect(a).to.eql([1,2,3]); pm.expect(r).to.equal(6); pm.expect([1,2,3].filter(x=>x>1)).to.eql([2,3]); pm.expect([{a:1},{a:2}].find(o=>o.a==2).a).to.equal(2); pm.expect([1,[2,3]].flat()).to.eql([1,2,3]); pm.expect([1,2,3].includes(2)).to.be.true; pm.expect([1,2].join('-')).to.equal('1-2');});", 1, 0, NULL);
    run("destructure", "const { data, total } = pm.response.json(); const [first, ...rest] = data; pm.test('d', ()=>{ pm.expect(total).to.equal(12); pm.expect(first.id).to.equal(7); pm.expect(rest).to.have.lengthOf(1); });", 1, 0, NULL);
    run("try/catch", "let m=''; try { null.x } catch (e) { m = e.message } finally { m += '!' } pm.test('t', ()=>pm.expect(m).to.include('Cannot read')); try { throw new Error('boom') } catch (e) { pm.test('e', ()=>pm.expect(e.message).to.equal('boom')) }", 2, 0, NULL);
    run("optional/nullish", "const j = pm.response.json(); pm.test('o', ()=>{ pm.expect(j.nope?.x?.y).to.be.undefined; pm.expect(j.nope ?? 5).to.equal(5); pm.expect(j.nested?.x.y[2]).to.equal(3); });", 1, 0, NULL);
    run("JSON/Math", "const s = JSON.stringify({a:[1,2],b:'x'}); pm.test('j', ()=>{ pm.expect(s).to.equal('{\"a\":[1,2],\"b\":\"x\"}'); pm.expect(JSON.parse(s).a[1]).to.equal(2); pm.expect(Math.max(1,5,3)).to.equal(5); pm.expect(Math.floor(2.7)).to.equal(2); pm.expect(Math.round(2.5)).to.equal(3); pm.expect(0.1+0.2).to.be.closeTo(0.3, 0.0001); pm.expect(String(0.1+0.2)).to.equal('0.30000000000000004'); pm.expect((1.005).toFixed(2)).to.be.a('string'); pm.expect(10/4).to.equal(2.5); pm.expect(7%3).to.equal(1); pm.expect(2**10).to.equal(1024); });", 1, 0, NULL);
    run("headers/cookies", "pm.test('h', ()=>{ pm.response.to.have.header('content-type'); pm.expect(pm.response.headers.get('Content-Type')).to.include('json'); pm.expect(pm.response.cookies.get('sid')).to.equal('abc'); pm.response.to.be.ok; pm.response.to.be.json; pm.response.to.not.be.error; pm.response.to.have.jsonBody('data[0].id', 7); });", 1, 0, NULL);
    run("not/negation", "pm.test('n', ()=>{ pm.expect(1).to.not.equal(2); pm.expect([1]).to.not.be.empty; pm.expect({}).to.be.empty; pm.expect('x').to.not.include('y'); pm.expect(5).to.be.within(1,10); pm.expect(2).to.be.oneOf([1,2]); });", 1, 0, NULL);
    run("legacy", "tests['status is 200'] = responseCode.code === 200; tests['body has Ana'] = responseBody.indexOf('Ana') >= 0; postman.setEnvironmentVariable('legacy', 'yes'); tests['env'] = postman.getEnvironmentVariable('legacy') === 'yes'; tests['falso'] = false;", 3, 1, NULL);
    run("request (pre)", "pm.request.headers.add({key:'X-A', value:'1'}); pm.request.headers.upsert({key:'x-a', value:'2'}); pm.request.url.query.add({key:'b', value:'3'}); pm.test('r', ()=>{ pm.expect(pm.request.headers.get('X-A')).to.equal('2'); pm.expect(pm.request.url.toString()).to.equal('http://x/users?page=1&b=3'); pm.expect(pm.request.url.getPath()).to.equal('/users'); pm.expect('' + pm.request.url).to.include('b=3'); });", 1, 0, NULL);
    run("replaceIn/dynamic", "const u = pm.variables.replaceIn('{{base}}/a/{{$randomInt}}/{{nope}}'); pm.test('r', ()=>{ pm.expect(u).to.include('http://x/a/'); pm.expect(u).to.include('{{nope}}'); pm.expect(pm.variables.replaceIn('{{$guid}}')).to.have.lengthOf(36); });", 1, 0, NULL);
    run("setNextRequest", "pm.execution.setNextRequest('Login'); postman.setNextRequest(null);", 0, 0, NULL);
    run("syntax error", "pm.test('x', () => {", 0, 0, "SyntaxError");
    run("regex", "const r = /ab+c/;", 0, 0, "regular expressions");
    run("ref error", "foo.bar", 0, 0, "ReferenceError: foo is not defined");
    run("infinite loop", "while(true){}", 0, 0, "too long");
    run("recursion", "function f(n){ return f(n+1) } f(0)", 0, 0, "call stack");
    run("switch/date/misc", "let k=''; switch(2){ case 1: k='a'; break; case 2: k='b'; case 3: k+='c'; break; default: k='d' } const d = new Date(0); pm.test('s', ()=>{ pm.expect(k).to.equal('bc'); pm.expect(d.toISOString()).to.equal('1970-01-01T00:00:00.000Z'); pm.expect(typeof Date.now()).to.equal('number'); pm.expect(parseInt('42px')).to.equal(42); pm.expect(parseFloat('3.5')).to.equal(3.5); pm.expect(isNaN('x')).to.be.true; pm.expect(Object.keys({a:1,b:2})).to.eql(['a','b']); pm.expect(btoa('cartero')).to.equal('Y2FydGVybw=='); pm.expect(atob('Y2FydGVybw==')).to.equal('cartero'); pm.expect(encodeURIComponent('a b&c')).to.equal('a%20b%26c'); pm.expect(Array.isArray([])).to.be.true; pm.expect([...'ab']).to.eql(['a','b']); pm.expect({...{a:1}, b:2}).to.eql({a:1,b:2}); });", 1, 0, NULL);
    run("console", "console.log('hola', {a:1}, [1,2], 3); console.warn('cuidado')", 0, 0, NULL);
    run("assert msg", "pm.test('m', ()=> pm.expect(404, 'el status').to.equal(200)); pm.test('s', () => pm.response.to.have.status(404));", 0, 2, NULL);
    run("arrow this & objects", "const o = { n: 3, get2() { return this.n * 2 }, f: function(){ return this.n } }; pm.test('o', ()=>{ pm.expect(o.get2()).to.equal(6); pm.expect(o.f()).to.equal(3); });", 1, 0, NULL);
    run("for in/of", "const o={a:1,b:2}; let ks=[]; for (const k in o) ks.push(k); let vs=0; for (const v of [1,2,3]) vs+=v; let e=[]; for (const [k,v] of Object.entries(o)) e.push(k+v); pm.test('f', ()=>{ pm.expect(ks).to.eql(['a','b']); pm.expect(vs).to.equal(6); pm.expect(e).to.eql(['a1','b2']); });", 1, 0, NULL);
    run("ASI", "const a = 1\nconst b = 2\nlet c = a +\n b\npm.test('asi', () => pm.expect(c).to.equal(3))\n", 1, 0, NULL);
    run("numbers fmt", "pm.test('n', ()=>{ pm.expect(String(1e21)).to.equal('1e+21'); pm.expect(String(123.456)).to.equal('123.456'); pm.expect(String(-0.000001)).to.equal('-0.000001'); pm.expect(String(1/3)).to.equal('0.3333333333333333'); pm.expect(String(2**53)).to.equal('9007199254740992'); pm.expect((255).toString(16)).to.equal('ff'); pm.expect((3.14159).toFixed(2)).to.equal('3.14'); });", 1, 0, NULL);
    printf("%s (%d failed)\n", fails ? "FAILURES" : "all good", fails);
    return fails != 0;
}
