FROM golang:1.11

RUN apt-get update
RUN apt-get install -y libjansson-dev
RUN apt-get install -y libreadline-dev

RUN mkdir -p /go/src/github.com/turbio/evaldb

WORKDIR /go/src/github.com/turbio/evaldb

COPY . .

RUN go get ./cmd/evalserver
RUN make

EXPOSE 5000

CMD ./evalserver
