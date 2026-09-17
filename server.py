"""UI transport: all generated text is emitted by RV32 firmware in RTL simulation."""
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import json
import subprocess
import threading
import time
ROOT=Path(__file__).resolve().parent
LOCK=threading.Lock()
def prepare_prompt(prompt):
    if not isinstance(prompt,str) or '\0' in prompt or len(prompt.encode())>255:
        raise ValueError('输入必须是不含 NUL、最多255字节的文本')
    normalized=prompt.strip()
    if not normalized:raise ValueError('请输入非空提示词')
    if len(normalized)>30:raise ValueError('提示词最多30个字符，请缩短后重试')
    known=set(json.loads((ROOT/'weights.json').read_text())['vocab'][3:])
    unknown=list(dict.fromkeys(c for c in normalized if c not in known))
    return normalized,unknown
class Handler(BaseHTTPRequestHandler):
    def respond(self,code,value,ctype='application/json; charset=utf-8'):
        data=json.dumps(value,ensure_ascii=False).encode() if not isinstance(value,bytes) else value
        self.send_response(code);self.send_header('Content-Type',ctype);self.send_header('Content-Length',str(len(data)))
        self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(data)
    def do_GET(self):
        if self.path=='/':self.respond(200,(ROOT/'index.html').read_bytes(),'text/html; charset=utf-8')
        elif self.path=='/api/info':
            self.respond(200,{'training':json.loads((ROOT/'training_report.json').read_text()),
                'verification':json.loads((ROOT/'verification_report.json').read_text()) if (ROOT/'verification_report.json').exists() else None})
        else:self.respond(404,{'error':'不存在的路径'})
    def do_POST(self):
        if self.path!='/api/generate':self.respond(404,{'error':'不存在的路径'});return
        try:
            length=int(self.headers.get('Content-Length','0'))
            if not 0<length<=4096:raise ValueError('请求长度不正确')
            data=json.loads(self.rfile.read(length))
            if not isinstance(data,dict):raise ValueError('请求需要是 JSON 对象')
            prompt=data.get('prompt');kind=data.get('mode','checked')
            original=prompt;prompt,unknown=prepare_prompt(prompt)
            if kind not in ['checked','fast','cpu_attention','cpu']:raise ValueError('无效运行模式')
            if not LOCK.acquire(blocking=False):self.respond(429,{'error':'仿真正在运行'});return
            try:
                started=time.perf_counter()
                p=subprocess.run([str(ROOT/'build/obj/Vsoc'),'+firmware='+str(ROOT/f'build/{kind}.hex'),'+weights='+str(ROOT/'external.hex'),'+ext_latency=3','--text',prompt],capture_output=True,text=True,timeout=120)
                try:d=json.loads(p.stdout)
                except json.JSONDecodeError:raise RuntimeError('仿真未输出有效结果')
                if p.returncode:
                    message=d.get('error','仿真失败')
                    message={'invalid UTF-8':'输入编码不是合法 UTF-8','empty prompt':'请输入非空提示词','prompt too long':'提示词最多30个字符'}.get(message,message)
                    self.respond(400 if p.returncode==2 else 500,{'error':message});return
                d['host_elapsed_ms']=round((time.perf_counter()-started)*1000,2)
                d['input_prompt']=original;d['effective_prompt']=prompt;d['unknown_characters']=unknown
                d['input_note']=('已清理首尾空白。' if original!=prompt else '')+('词表外字符已由 RISC-V 编码为 UNK；模型无法区分这些字符的含义。' if unknown else '')
                self.respond(200,d)
            finally:LOCK.release()
        except (ValueError,UnicodeError) as e:self.respond(400,{'error':str(e)})
        except subprocess.TimeoutExpired:self.respond(504,{'error':'仿真超时'})
        except (RuntimeError,OSError) as e:self.respond(500,{'error':str(e)})
if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument('--port',type=int,default=8767);args=parser.parse_args()
    print(f'Transformer SoC: http://127.0.0.1:{args.port}',flush=True)
    ThreadingHTTPServer(('127.0.0.1',args.port),Handler).serve_forever()
