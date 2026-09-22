// Decode Remastered sections without rewriting commands, unit IDs or terrain.
// The output is an internal, uncompressed OpenBW adapter format, not a .rep.
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"github.com/icza/screp/repparser/repdecoder"
	"io"
	"os"
)

const maxSection = 64 * 1024 * 1024

func convertMap(chk []byte) ([]byte, error) {
	var out bytes.Buffer
	versionSeen, terrainSeen := false, false
	for offset := 0; offset < len(chk); {
		if len(chk)-offset < 8 {
			return nil, fmt.Errorf("truncated CHK header")
		}
		id := string(chk[offset : offset+4])
		n := int(binary.LittleEndian.Uint32(chk[offset+4 : offset+8]))
		offset += 8
		if n > len(chk)-offset {
			return nil, fmt.Errorf("truncated CHK %s", id)
		}
		data := chk[offset : offset+n]
		offset += n
		if id == "VER " {
			if versionSeen || len(data) != 2 {
				return nil, fmt.Errorf("invalid/duplicate VER")
			}
			versionSeen = true
			v := binary.LittleEndian.Uint16(data)
			if v != 59 && v != 63 && v != 64 && v != 205 && v != 206 {
				return nil, fmt.Errorf("unsupported CHK version %d", v)
			}
			if v == 206 {
				data = []byte{205, 0}
			}
			if v == 64 {
				data = []byte{63, 0}
			}
		}
		if id == "ERA " {
			if terrainSeen || len(data) != 2 {
				return nil, fmt.Errorf("invalid/duplicate ERA")
			}
			terrainSeen = true
			v := binary.LittleEndian.Uint16(data)
			if v > 7 || v == 2 || v == 3 {
				return nil, fmt.Errorf("unvalidated tileset %d", v)
			}
		}
		if id == "STRx" {
			if len(data) < 4 {
				return nil, fmt.Errorf("truncated STRx")
			}
			count := int(binary.LittleEndian.Uint32(data))
			if count > 32766 || 4+4*count > len(data) {
				return nil, fmt.Errorf("invalid STRx count")
			}
			converted := make([]byte, 2+2*count)
			binary.LittleEndian.PutUint16(converted, uint16(count))
			for i := 0; i < count; i++ {
				start := int(binary.LittleEndian.Uint32(data[4+i*4 : 8+i*4]))
				if start >= len(data) {
					return nil, fmt.Errorf("invalid STRx offset")
				}
				end := bytes.IndexByte(data[start:], 0)
				if end < 0 || len(converted)+end+1 > 65535 {
					return nil, fmt.Errorf("STRx string cannot fit legacy table")
				}
				binary.LittleEndian.PutUint16(converted[2+i*2:4+i*2], uint16(len(converted)))
				converted = append(converted, data[start:start+end+1]...)
			}
			id, data = "STR ", converted
		}
		out.WriteString(id)
		binary.Write(&out, binary.LittleEndian, uint32(len(data)))
		out.Write(data)
	}
	if !versionSeen || !terrainSeen {
		return nil, fmt.Errorf("missing VER/ERA")
	}
	return out.Bytes(), nil
}

func decode(path string) ([]byte, map[string]any, error) {
	d, err := repdecoder.NewFromFile(path)
	if err != nil {
		return nil, nil, err
	}
	defer d.Close()
	sections := make([][]byte, 0)
	var limits []byte
	for i := 0; ; i++ {
		err = d.NewSection()
		if err == repdecoder.ErrNoMoreSections {
			break
		}
		if err != nil {
			return nil, nil, err
		}
		var size int32
		if i == 0 {
			size = 4
		} else if i == 1 {
			size = 633
		} else if i == 4 {
			size = 768
		}
		if i == 2 || i == 3 {
			b, _, e := d.Section(4)
			if e != nil || len(b) != 4 {
				return nil, nil, fmt.Errorf("invalid section length: %v", e)
			}
			length := binary.LittleEndian.Uint32(b)
			if length > maxSection {
				return nil, nil, fmt.Errorf("section exceeds 64 MiB limit")
			}
			size = int32(length)
		}
		b, id, e := d.Section(size)
		// Modern NewSection does not inspect EOF; Section reads the next ID.
		if i >= 5 && e == io.EOF && id == 0 {
			break
		}
		if e != nil {
			return nil, nil, e
		}
		if i < 5 {
			sections = append(sections, b)
		}
		if id == 1398033740 {
			if limits != nil {
				return nil, nil, fmt.Errorf("duplicate LMTS")
			}
			limits = b
		}
	}
	if len(sections) != 5 || len(sections[0]) != 4 || string(sections[0]) != "seRS" || len(sections[1]) != 633 || len(limits) != 28 {
		return nil, nil, fmt.Errorf("expected Remastered seRS header and LMTS")
	}
	// This adapter is deliberately restricted to the audited corpus format.
	if binary.LittleEndian.Uint32(limits[12:16]) != 3400 {
		return nil, nil, fmt.Errorf("unsupported unit limit")
	}
	for i := 0; i < 28; i += 4 {
		n := binary.LittleEndian.Uint32(limits[i : i+4])
		if n == 0 || n > 100000 {
			return nil, nil, fmt.Errorf("invalid LMTS capacity")
		}
	}
	convertedMap, err := convertMap(sections[3])
	if err != nil {
		return nil, nil, err
	}
	var output bytes.Buffer
	binary.Write(&output, binary.LittleEndian, uint32(0x53526577))
	output.Write(limits)
	output.Write(sections[1])
	for _, data := range [][]byte{sections[2], convertedMap} {
		binary.Write(&output, binary.LittleEndian, uint32(len(data)))
		output.Write(data)
	}
	metadata := map[string]any{
		"decoder": "screp-v1.13.4", "unit_limit": 3400,
		"commands_sha256":     fmt.Sprintf("%x", sha256.Sum256(sections[2])),
		"original_map_sha256": fmt.Sprintf("%x", sha256.Sum256(sections[3])),
		"decoded_sha256":      fmt.Sprintf("%x", sha256.Sum256(output.Bytes())),
		"training_ready":      false,
	}
	return output.Bytes(), metadata, nil
}

func run() error {
	if len(os.Args) != 3 {
		return fmt.Errorf("usage: replay_decode INPUT.rep OUTPUT.raw")
	}
	output, metadata, err := decode(os.Args[1])
	if err != nil {
		return err
	}
	file, err := os.OpenFile(os.Args[2], os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	_, writeErr := file.Write(output)
	closeErr := file.Close()
	if writeErr != nil {
		return writeErr
	}
	if closeErr != nil {
		return closeErr
	}
	return json.NewEncoder(os.Stdout).Encode(metadata)
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
