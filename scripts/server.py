#!/usr/bin/env python3
"""Local UI transport only: inference and tool replies come from RV32 firmware."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
LOCK = threading.Lock()
class Handler(BaseHTTPRequestHandler):
    def respond(self, status, data, content_type='application/json; charset=utf-8'):
        if isinstance(data,dict): data=json.dumps(data,ensure_ascii=False).encode()
        self.send_response(status);self.send_header('Content-Type',content_type)
        self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store')
        self.end_headers();self.wfile.write(data)

    def do_GET(self):
        if self.path in ['/', '/index.html']:
            self.respond(200,(ROOT/'web/index.html').read_bytes(),'text/html; charset=utf-8')
        elif self.path=='/api/info':
            ds=json.loads((ROOT/'model/training_report.json').read_text())
            report_path=ROOT/'docs/verification_results.json'
            report=json.loads(report_path.read_text()) if report_path.exists() else None
            self.respond(200,{'model':{'shape':[256,32,6],'weight_bytes':8384,'bias_bytes':152},
                'training':{k:{a:b for a,b in ds[k].items() if a!='cases'} for k in ['train','validation','test','ood_test']},
                'verification':{k:v for k,v in report.items() if k!='cases'} if report else None})
        else:self.respond(404,{'error':'页面不存在'})

    def do_POST(self):
        if self.path!='/api/infer':self.respond(404,{'error':'接口不存在'});return
        try:
            size=int(self.headers.get('Content-Length','0'))
            if size<=0 or size>4096:raise ValueError('请求长度不正确')
            data=json.loads(self.rfile.read(size))
            if not isinstance(data,dict):raise ValueError('请提交 JSON 对象')
            text=data.get('text')
            if not isinstance(text,str) or '\0' in text or len(text.encode('utf-8'))>255:raise ValueError('请输入不超过255个UTF-8字节的文本')
            if not LOCK.acquire(blocking=False):self.respond(429,{'error':'仿真正在运行，请稍后重试'});return
            try:
                started=time.perf_counter()
                result=subprocess.run([str(ROOT/'build/obj/Vsoc'),'+firmware='+str(ROOT/'build/firmware.hex'),'--text',text],cwd=ROOT,
                    capture_output=True,text=True,timeout=30,check=True)
                payload=json.loads(result.stdout)
                if not payload.get('match'):raise RuntimeError('软件与硬件结果不一致')
                payload['host_elapsed_ms']=round((time.perf_counter()-started)*1000,2)
                self.respond(200,payload)
            finally:LOCK.release()
        except (ValueError,UnicodeError,json.JSONDecodeError) as exc:self.respond(400,{'error':str(exc)})
        except subprocess.TimeoutExpired:self.respond(504,{'error':'RTL仿真超时'})
        except (subprocess.CalledProcessError,RuntimeError,FileNotFoundError) as exc:
            self.respond(500,{'error':'仿真执行失败，请在终端运行 make test 检查','detail':str(exc)})

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--port',type=int,default=8765);args=parser.parse_args()
    server=ThreadingHTTPServer(('127.0.0.1',args.port),Handler)
    print(f'RISC-V INT8 Assistant: http://127.0.0.1:{args.port}',flush=True)
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:server.server_close()
