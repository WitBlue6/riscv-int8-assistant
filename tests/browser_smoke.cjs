// Optional UI test. Requires Playwright (or playwright-core) and a Chromium browser.
const assert = require('node:assert/strict');
const path = require('node:path');
const {spawn} = require('node:child_process');
const fs = require('node:fs');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(__dirname, '..');
(async()=>{
  const server = spawn(process.env.PYTHON || 'python3', ['scripts/server.py','--port','8766'], {cwd:root, stdio:'pipe'});
  let browser;
  try {
    await new Promise((resolve,reject)=>{server.stdout.once('data',resolve);server.once('error',reject);server.once('exit',c=>reject(new Error('server exited '+c)));});
    browser = await chromium.launch({headless:true, executablePath:process.env.BROWSER_EXECUTABLE || undefined});
    const page = await browser.newPage({viewport:{width:1440,height:1150},deviceScaleFactor:1});
    const errors=[];page.on('pageerror',e=>errors.push(e.message));
    await page.goto('http://127.0.0.1:8766');
    await page.waitForFunction(()=>document.getElementById('verification').textContent.includes('387'));
    await page.locator('[data-text="帮我算一下12加30"]').click();
    await page.waitForFunction(()=>document.getElementById('chat').textContent.includes('结果：42'));
    assert.equal(await page.locator('#speed').textContent(),'5.08×');
    assert.equal(await page.locator('#resultstatus').textContent(),'逐层结果一致');
    await page.waitForTimeout(600); // let the 400 ms chart transition finish before capture
    await page.screenshot({path:path.join(root,'build/ui-desktop.png'),fullPage:true});
    await page.locator('[data-text="清零统计"]').click();
    await page.waitForFunction(()=>document.getElementById('registers').textContent.includes('执行后 0'));
    await page.locator('#text').fill('今天会下雨吗');await page.locator('#send').click();
    await page.waitForFunction(()=>document.getElementById('decision').textContent.includes('未接受'));
    await page.locator('#text').fill('你'.repeat(86));await page.locator('#send').click();
    assert.match(await page.locator('#error').textContent(),/255/);
    const bad=await page.request.post('http://127.0.0.1:8766/api/infer',{data:{text:'你'.repeat(86)}});assert.equal(bad.status(),400);
    const invalid=await page.request.post('http://127.0.0.1:8766/api/infer',{data:{text:123}});assert.equal(invalid.status(),400);
    await page.setViewportSize({width:390,height:844});
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'mobile horizontal overflow');
    await page.screenshot({path:path.join(root,'build/ui-mobile.png'),fullPage:true});
    assert.deepEqual(errors,[]);
    const result={passed:true,checks:['calculation=42','cycle display','software/RTL match','counter reset','unknown input rejection','255-byte limit','invalid API input','mobile layout','no page errors']};
    fs.writeFileSync(path.join(root,'docs/ui_test_results.json'),JSON.stringify(result,null,2)+'\n');
    console.log(JSON.stringify(result));
  } finally {if(browser)await browser.close();server.kill();}
})().catch(e=>{console.error(e);process.exitCode=1;});
