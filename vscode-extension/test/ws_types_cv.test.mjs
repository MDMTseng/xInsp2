// ws_types_cv.test.mjs — Vec <-> OpenCV interop (xi_types_cv.hpp).
//
// Convert a nominal Vec to a cv::Vec, run cv matrix/vector math, convert back.

import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');
const SCRIPT = `#include <xi/xi.hpp>            // OpenCV
#include <xi/xi_use.hpp>
#include <xi/xi_types.hpp>
#include <xi/xi_types_cv.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
  xi::Vec3 v(1, 2, 3);
  cv::Vec3d cvv = xi::to_cv(v);

  // matrix * vector: scale by 2 then add a row-mixing term
  cv::Matx33d M(2,0,0,  0,2,0,  1,0,2);
  cv::Vec3d r = M * cvv;                 // (2, 4, 1*1 + 2*3) = (2,4,7)
  xi::Vec3 out = xi::from_cv(r);
  VAR(rx, out.x());
  VAR(ry, out.y());
  VAR(rz, out.z());

  // dot/norm via cv on a Vec2
  xi::Vec2 a(3, 4);
  VAR(norm, cv::norm(xi::to_cv(a)));     // 5

  // round-trip
  xi::Vec4 q(1.5, 2.5, 3.5, 4.5);
  xi::Vec4 q2 = xi::from_cv(xi::to_cv(q));
  VAR(q2_z, q2.z());                     // 3.5

  // MatN <-> cv::Matx
  xi::Mat3 m;                            // identity
  VAR(m00, m.at(0,0));                   // 1
  VAR(m01, m.at(0,1));                   // 0
  m.set(0,0, 2).set(1,1, 3);             // scale x by 2, y by 3
  cv::Matx33d Mx = xi::to_cv(m);
  cv::Vec3d mr = Mx * cv::Vec3d(4, 5, 6);   // (8, 15, 6)
  xi::Vec3 mout = xi::from_cv(mr);
  VAR(mrx, mout.x());
  VAR(mry, mout.y());
  VAR(mrz, mout.z());
  xi::Mat3 m2 = xi::from_cv(cv::Matx33d(cv::Matx33d::eye() * 7));
  VAR(m2_11, m2.at(1,1));                // 7
}`;

async function waitRsp(c, id) { for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === id) return m; } }

test('Vec <-> cv: matrix*vector, norm, round-trip', { skip: 'vars/preview/subscribe removed with VAR (branch refactor/remove-var-core) — pending preview plugin (observed via vars)' }, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xi_cv_'));
    const sp = join(dir, 'inspect.cpp');
    writeFileSync(sp, SCRIPT);
    await withBackend(async (c) => {
        await c.nextText();
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: slash(sp) } });
        assert.equal((await waitRsp(c, 1)).ok, true, 'compile ok (xi_types_cv pulls OpenCV)');
        c.send({ type: 'cmd', id: 2, name: 'run' });
        await waitRsp(c, 2);
        let vars = null;
        for (;;) { const m = await c.nextText(); if (m.type === 'vars') { vars = m.items; break; } }
        const v = Object.fromEntries(vars.map(x => [x.name, x.value]));
        console.log('vars:', JSON.stringify(v));
        assert.equal(v.rx, 2);
        assert.equal(v.ry, 4);
        assert.equal(v.rz, 7, 'M * v via OpenCV, back to xi::Vec3');
        assert.equal(v.norm, 5, 'cv::norm of a converted Vec2');
        assert.equal(v.q2_z, 3.5, 'Vec4 round-trips through cv');
        assert.equal(v.m00, 1, 'Mat3 defaults to identity');
        assert.equal(v.m01, 0);
        assert.equal(v.mrx, 8, 'Mat3 -> cv::Matx33d, M*v');
        assert.equal(v.mry, 15);
        assert.equal(v.mrz, 6);
        assert.equal(v.m2_11, 7, 'from_cv(cv::Matx) -> Mat3');
    });
});
