package main

import (
	"encoding/json"
	"errors"
	"strconv"
	"sync"

	"github.com/boltdb/bolt"
	log "github.com/sirupsen/logrus"
)

var errDBExists = errors.New("db already exists")
var errDBNotExists = errors.New("db does not exist")

var dbsBucket = []byte("dbs")
var linksBucket = []byte("links")
var d2hBucket = []byte("d2h")
var h2dBucket = []byte("h2d")

var db *bolt.DB

func dbEvaler(dbid string) (string, error) {
	lang := ""

	err := db.View(func(tx *bolt.Tx) error {
		rdbs := tx.Bucket(dbsBucket)
		edb := rdbs.Bucket([]byte(dbid))

		lang = string(edb.Get([]byte("lang")))

		return nil
	})
	if err != nil {
		return "", err
	}

	return lang, nil
}

func hasDB(dbid string) bool {
	found := false

	db.View(func(tx *bolt.Tx) error {
		rdbs := tx.Bucket(dbsBucket)
		found = rdbs.Bucket([]byte(dbid)) != nil
		return nil
	})

	return found
}

var logsLock sync.RWMutex
var logcbs = map[string][]chan *transac{}

func tailDB(dbid string, s chan *transac) error {
	err := db.View(func(tx *bolt.Tx) error {
		rdbs := tx.Bucket(dbsBucket)
		dbb := rdbs.Bucket([]byte(dbid))
		if dbb == nil {
			return errors.New("internal error")
		}

		logs := dbb.Bucket([]byte("logs"))
		if logs == nil {
			return errors.New("internal error")
		}

		c := logs.Cursor()
		for k, v := c.First(); k != nil; k, v = c.Next() {
			var t transac
			if json.Unmarshal(v, &t) == nil {
				s <- &t
			}
		}

		return nil
	})
	if err != nil {
		return err
	}

	logsLock.Lock()
	cbs, ok := logcbs[dbid]
	if !ok {
		cbs = []chan *transac{}
	}
	logcbs[dbid] = append(cbs, s)
	logsLock.Unlock()

	return nil
}

func unTailDB(ch chan *transac) {
	logsLock.Lock()
	defer logsLock.Unlock()

	for dbid, chs := range logcbs {
		for j, dbch := range chs {
			if dbch == ch {
				logcbs[dbid] = append(chs[:j], chs[j+1:]...)
				return
			}
		}
	}
}

func createDB(dbid, lang string) error {
	return db.Update(func(tx *bolt.Tx) error {
		rdbs := tx.Bucket(dbsBucket)
		exists := rdbs.Bucket([]byte(dbid)) != nil
		if exists {
			return errDBExists
		}

		newDB, err := rdbs.CreateBucket([]byte(dbid))
		if err != nil {
			return err
		}

		err = newDB.Put([]byte("lang"), []byte(lang))
		if err != nil {
			return err
		}

		_, err = newDB.CreateBucket([]byte("logs"))
		return err
	})
}

func dbForLink(host string) (string, error) {
	dbname := ""

	err := db.View(func(tx *bolt.Tx) error {
		links := tx.Bucket(linksBucket)
		if links == nil {
			panic("ohno")
		}

		h2d := links.Bucket([]byte("h2d"))
		if h2d == nil {
			panic("ohno")
		}

		dbid := h2d.Get([]byte(host))
		if dbid == nil {
			return errDBNotExists
		}

		dbname = string(dbid)

		return nil
	})
	if err != nil {
		return "", err
	}

	return dbname, nil
}

func linkForDB(dbid string) (string, error) {
	host := ""

	err := db.View(func(tx *bolt.Tx) error {
		host = string(tx.Bucket(linksBucket).Bucket(d2hBucket).Get([]byte(dbid)))
		return nil
	})
	if err != nil {
		return "", err
	}

	return host, nil
}

func setLink(dbid, host string) error {
	return db.Update(func(tx *bolt.Tx) error {
		dbb := tx.Bucket(dbsBucket).Bucket([]byte(dbid))
		if dbb == nil {
			return errDBNotExists
		}

		existing := tx.Bucket(linksBucket).Bucket(d2hBucket).Get([]byte(dbid))
		if existing != nil {
			tx.Bucket(linksBucket).Bucket(d2hBucket).Delete([]byte(dbid))
			tx.Bucket(linksBucket).Bucket(h2dBucket).Delete([]byte(existing))
		}

		if host == "" {
			return nil
		}

		if err := tx.Bucket(linksBucket).Bucket(d2hBucket).Put([]byte(dbid), []byte(host)); err != nil {
			return err
		}

		if err := tx.Bucket(linksBucket).Bucket(h2dBucket).Put([]byte(host), []byte(dbid)); err != nil {
			return err
		}

		return nil
	})
}

func logTransac(dbid string, t *transac) error {
	err := db.Update(func(tx *bolt.Tx) error {
		dbb := tx.Bucket(dbsBucket).Bucket([]byte(dbid))
		if dbb == nil {
			return errDBNotExists
		}

		logs := dbb.Bucket([]byte("logs"))
		if logs == nil {
			return errors.New("internal error")
		}

		j, err := json.Marshal(t)
		if err != nil {
			return err
		}

		s, _ := logs.NextSequence()

		return logs.Put([]byte(strconv.FormatUint(s, 10)+".t"), j)
	})

	if err != nil {
		return err
	}

	logsLock.Lock()
	cbs, ok := logcbs[dbid]
	if ok {
		for _, s := range cbs {
			s <- t
		}
	}
	logsLock.Unlock()

	return nil
}

func openDB(path string) {
	var err error
	db, err = bolt.Open(path, 0666, nil)
	if err != nil {
		log.Fatalln("unable to open global db:", err)
	}

	err = db.Update(func(tx *bolt.Tx) error {

		dbb, err := tx.CreateBucketIfNotExists(dbsBucket)
		if err != nil {
			return err
		}

		links, err := tx.CreateBucketIfNotExists(linksBucket)
		if err != nil {
			return err
		}

		if _, err := links.CreateBucketIfNotExists(d2hBucket); err != nil {
			return err
		}

		if _, err := links.CreateBucketIfNotExists(h2dBucket); err != nil {
			return err
		}

		dbb.ForEach(func(k []byte, v []byte) error {
			d := dbb.Bucket(k)
			d.ForEach(func(k []byte, v []byte) error {
				if string(k) == "logs" {
					bb := d.Bucket(k)
					bb.ForEach(func(k []byte, v []byte) error {
						return nil
					})
				}

				return nil
			})

			return nil
		})

		return nil
	})
	if err != nil {
		log.Fatalln("cannot setup db:", err)
	}
}
