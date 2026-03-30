#!/usr/bin/env python3
import sys, json, argparse, requests
from ddgs import DDGS
from markdownify import markdownify as md

def search(q, max_results=10):
    return list(DDGS().text(q, max_results=max_results))

def html2md(url, timeout=10):
    return md(requests.get(url, headers={"User-Agent":"Mozilla/5.0"}, timeout=timeout).text)

def fmt_results(results, fmt="text"):
    if fmt=="json":
        return json.dumps(results, indent=2, ensure_ascii=False)
    elif fmt=="table":
        return "\n".join([f"{i+1}. {r['title'][:60]} | {r['href'][:50]} | {r['body'][:80]}..." for i,r in enumerate(results)])
    else:
        return "\n\n".join([f"{r['title']}\n{r['href']}\n{r['body']}" for r in results])

def main():
    p=argparse.ArgumentParser()
    p.add_argument("query",nargs="?",help="Search query or URL")
    p.add_argument("--search","-s",action="store_true",help="Perform web search")
    p.add_argument("--html2md","-m",action="store_true",help="Convert HTML to Markdown")
    p.add_argument("--max_results","-n",type=int,default=10,help="Max search results (default: 10)")
    p.add_argument("--format","-f",choices=["text","json","table"],default="text",help="Output format")
    p.add_argument("--filter","-F",help="Filter results by keyword in title/body")
    p.add_argument("--sort","-S",choices=["relevance","title"],default="relevance",help="Sort results")
    p.add_argument("--timeout",type=int,default=10,help="Request timeout")
    p.add_argument("--output","-o",help="Output to file")
    args=p.parse_args()

    if not args.query:
        p.print_help(); sys.exit(1)

    try:
        if args.search or not args.html2md:
            results=search(args.query,max_results=args.max_results)
            if args.filter:
                results=[r for r in results if args.filter.lower() in r.get("title","").lower() or args.filter.lower() in r.get("body","").lower()]
            if args.sort=="title":
                results=sorted(results,key=lambda x:x.get("title",""))
            out=fmt_results(results,args.format)
        elif args.html2md:
            out=html2md(args.query,args.timeout)
        else:
            sys.exit("Specify --search or --html2md")
        
        if args.output:
            with open(args.output,"w",encoding="utf-8") as f: f.write(out)
        else:
            print(out)
    except Exception as e:
        sys.exit(f"Error: {e}")

if __name__=="__main__":
    main()
