//
//   date  : 2014-05-23 17:35
//   author: xjdrew
//

package main

import (
	"encoding/json"
	"flag"
	"io"
	"log"
	"math/rand"
	"net"
	"os"
	"os/signal"
	"runtime"
	"sync"
	"syscall"
	"time"
)

type Host struct {
	Addr   string
	Weight int
}

type Backend struct {
	TrafficUrl string
	Hosts      []Host
	weight     int
}

type Options struct {
	config  string
	backend Backend
}

var options Options
var backendMu sync.RWMutex

func usage() {
	log.Printf("usage: %s config\n", os.Args[0])
	flag.PrintDefaults()
	os.Exit(1)
}

func reloadConfig() error {
	fp, err := os.Open(options.config)
	if err != nil {
		return err
	}
	defer fp.Close()

	var backend Backend
	dec := json.NewDecoder(fp)
	err = dec.Decode(&backend)
	if err != nil {
		return err
	}

	for i := range backend.Hosts {
		host := &backend.Hosts[i]
		backend.weight += host.Weight
	}

	log.Printf("config:%v", backend)
	backendMu.Lock()
	options.backend = backend
	backendMu.Unlock()
	return nil
}

func chooseHost(weight int, hosts []Host) *Host {
	if weight <= 0 {
		return nil
	}

	v := rand.Intn(weight)
	for i := range hosts {
		if v < hosts[i].Weight {
			return &hosts[i]
		}
		v -= hosts[i].Weight
	}
	return nil
}

func forward(source *net.TCPConn, dest *net.TCPConn) {
	defer dest.CloseWrite()
	defer source.CloseRead()
	io.Copy(dest, source)
}

func handleConn(source *net.TCPConn) {
	backendMu.RLock()
	weight := options.backend.weight
	hosts := options.backend.Hosts
	backendMu.RUnlock()

	host := chooseHost(weight, hosts)
	if host == nil {
		source.Close()
		log.Println("choose host failed")
		return
	}

	dest, err := net.DialTimeout("tcp", host.Addr, 5*time.Second)
	if err != nil {
		source.Close()
		log.Printf("connect to %s failed: %s", host.Addr, err.Error())
		return
	}
	destTCP := dest.(*net.TCPConn)

	source.SetKeepAlive(true)
	source.SetKeepAlivePeriod(time.Second * 60)
	destTCP.SetKeepAlive(true)
	destTCP.SetKeepAlivePeriod(time.Second * 60)

	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		forward(source, destTCP)
	}()
	go func() {
		defer wg.Done()
		forward(destTCP, source)
	}()
	wg.Wait()

	source.Close()
	destTCP.Close()
}

const SIG_RELOAD = syscall.Signal(35)
const SIG_STATUS = syscall.Signal(36)

func status() {
	log.Printf("num goroutines: %d", runtime.NumGoroutine())
	buf := make([]byte, 32768)
	runtime.Stack(buf, true)
	log.Printf("!!!!!stack!!!!!:%s", buf)
}

func reload() {
	err := reloadConfig()
	if err != nil {
		log.Printf("reload failed:%v", err)
	} else {
		log.Printf("reload succeed")
	}
}

func init() {
	rand.Seed(time.Now().Unix())
}

func main() {
	var laddr string
	flag.StringVar(&laddr, "listen", ":1248", "local listen port")
	flag.Usage = usage
	flag.Parse()

	args := flag.Args()
	if len(args) < 1 {
		log.Println("config file is missed.")
		return
	}

	options.config = args[0]
	if err := reloadConfig(); err != nil {
		log.Printf("load config failed:%v", err)
		return
	}

	// run
	ln, err := net.Listen("tcp", laddr)
	if err != nil {
		log.Printf("build listener failed:%s", err.Error())
		return
	}
	defer ln.Close()

	go func() {
		c := make(chan os.Signal, 1)
		signal.Notify(c, SIG_RELOAD, SIG_STATUS, syscall.SIGTERM, syscall.SIGHUP)

		for sig := range c {
			switch sig {
			case SIG_RELOAD:
				reload()
			case SIG_STATUS:
				status()
			default:
				log.Printf("catch siginal: %v, ignored", sig)
			}
		}
	}()

	// run loop
	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("accept failed:%s", err.Error())
			if opErr, ok := err.(*net.OpError); ok {
				if !opErr.Temporary() {
					break
				}
			}
			continue
		}
		go handleConn(conn.(*net.TCPConn))
	}
}
