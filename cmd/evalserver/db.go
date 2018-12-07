package main

import (
	"encoding/json"
	"errors"
	"log"
	"strconv"

	"github.com/boltdb/bolt"
)

var dbsBucket = []byte("dbs")

var db *bolt.DB

func createDB(dbid string) error {
	return db.Update(func(tx *bolt.Tx) error {
		rdbs := tx.Bucket(dbsBucket)
		exists := rdbs.Bucket([]byte(dbid)) != nil
		if exists {
			return errors.New("db already exists")
		}

		_, err := rdbs.CreateBucket([]byte(dbid))
		return err
	})
}

func logQuery(dbid string, q *query) (string, error) {
	seqs := ""

	return seqs, db.Update(func(tx *bolt.Tx) error {
		dbb := tx.Bucket(dbsBucket).Bucket([]byte(dbid))
		if dbb == nil {
			return errors.New("db does not exist")
		}

		seq, _ := dbb.NextSequence()
		seqs = strconv.FormatUint(seq, 10)

		j, err := json.Marshal(q)
		if err != nil {
			return err
		}

		return dbb.Put([]byte(seqs+".q"), j)
	})
}

func logResult(dbid string, r *queryResult) error {
	return db.Update(func(tx *bolt.Tx) error {
		dbb := tx.Bucket(dbsBucket).Bucket([]byte(dbid))
		if dbb == nil {
			return errors.New("db does not exist")
		}

		j, err := json.Marshal(r)
		if err != nil {
			return err
		}

		return dbb.Put([]byte(r.Seq+".r"), j)
	})
}

func openDB() {
	var err error
	db, err = bolt.Open("./db/global", 0666, nil)
	if err != nil {
		log.Fatalln("unable to open global db:", err)
	}

	err = db.Update(func(tx *bolt.Tx) error {
		dbb, errr := tx.CreateBucketIfNotExists(dbsBucket)
		if errr != nil {
			return errr
		}

		dbb.ForEach(func(k []byte, v []byte) error {
			d := dbb.Bucket(k)
			log.Printf("db: %s with %d keys", k, d.Stats().KeyN)

			d.ForEach(func(k []byte, v []byte) error {
				log.Printf("\t%s: %s", k, v)
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
