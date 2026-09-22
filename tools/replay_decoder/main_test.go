package main

import (
	"bytes"
	"encoding/binary"
	"testing"
)

func chunk(id string, data []byte) []byte {
	var out bytes.Buffer
	out.WriteString(id)
	binary.Write(&out, binary.LittleEndian, uint32(len(data)))
	out.Write(data)
	return out.Bytes()
}

func fixture(version, terrain uint16) []byte {
	out := chunk("VER ", []byte{byte(version), byte(version >> 8)})
	return append(out, chunk("ERA ", []byte{byte(terrain), byte(terrain >> 8)})...)
}

func TestSupportedMapVersionsPreserveTerrainAndUnits(t *testing.T) {
	for _, version := range []uint16{59, 63, 64, 205, 206} {
		data := fixture(version, 4)
		tail := append(chunk("MTXM", []byte{255, 127, 8, 0}), chunk("UNIT", []byte{1, 2, 3, 4})...)
		data = append(data, tail...)
		got, err := convertMap(data)
		if err != nil {
			t.Fatal(err)
		}
		expected := version
		if version == 206 {
			expected = 205
		}
		if version == 64 {
			expected = 63
		}
		if binary.LittleEndian.Uint16(got[8:10]) != expected || !bytes.HasSuffix(got, tail) {
			t.Fatalf("map version %d rewrote simulation data", version)
		}
	}
}

func TestExtendedDisplayStrings(t *testing.T) {
	// One string, header offset 8, including its terminating NUL.
	strings := []byte{1, 0, 0, 0, 8, 0, 0, 0, 'M', 'a', 'p', 0}
	data := append(fixture(206, 4), chunk("STRx", strings)...)
	got, err := convertMap(data)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.HasSuffix(got, chunk("STR ", []byte{1, 0, 4, 0, 'M', 'a', 'p', 0})) {
		t.Fatal("incorrect STRx offset conversion")
	}
}

func TestRejectUnvalidatedOrMalformedMaps(t *testing.T) {
	cases := [][]byte{
		fixture(207, 4), fixture(206, 2), fixture(206, 3), fixture(206, 8),
		append(fixture(206, 4), 'x'),
		append(fixture(206, 4), chunk("VER ", []byte{205, 0})...),
		append(fixture(206, 4), chunk("STRx", []byte{255, 255, 255, 255})...),
		append(fixture(206, 4), chunk("STRx", []byte{1, 0, 0, 0, 100, 0, 0, 0})...),
		append(fixture(206, 4), chunk("STRx", []byte{1, 0, 0, 0, 8, 0, 0, 0, 'x'})...),
		chunk("VER ", []byte{206, 0}),
		append(fixture(206, 4), []byte{'U', 'N', 'I', 'T', 255, 255, 255, 255}...),
	}
	for i, data := range cases {
		if _, err := convertMap(data); err == nil {
			t.Errorf("case %d accepted invalid map", i)
		}
	}
}
